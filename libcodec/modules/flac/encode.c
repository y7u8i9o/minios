/* FLAC encoding in blocks of 4096 samples. For each channel the encoder
 * computes the cost of a CONSTANT, a VERBATIM, the best FIXED and several
 * LPC subframes and writes the cheapest. LPC coefficients come from the
 * autocorrelation of the block under a Tukey window and the
 * Levinson-Durbin recursion, for orders up to 32. The residual of every
 * predictor is coded with the partition order, the Rice parameters and
 * the escaped partitions that give the fewest bits. A stereo frame uses
 * the cheapest of the four channel assignments. STREAMINFO records the
 * frame size limits and the MD5 sum of the samples. */
#include "flac.h"
#include <math.h>

#define BLOCK 4096
#define MAX_PORDER 8
#define LPC_TRIES 3                     /* LPC orders that are coded exactly */

/* A coded residual: the method, the partition order, and for every
 * partition its Rice parameter or, with ESCAPED set, the width of its
 * raw samples. */
#define ESCAPED 0x100
struct coding {
    unsigned method, porder;
    unsigned param[1u << MAX_PORDER];
    uint64_t bits;
};

/* The subframe chosen for one channel signal. */
enum { SUB_CONSTANT, SUB_VERBATIM, SUB_FIXED, SUB_LPC };
struct subframe {
    int type;
    unsigned bits;                      /* sample size after the wasted bits */
    unsigned wasted, order, precision;
    int shift;
    int32_t coef[FLAC_MAX_ORDER];
    struct coding coding;
    uint64_t cost;                      /* bits of the whole subframe */
    int64_t *x;                         /* the signal shifted by the wasted bits */
    int64_t *res;                       /* the residual of a predictor */
};

/* Work memory for one channel signal. */
struct work {
    int64_t x[BLOCK], res[BLOCK], trial[BLOCK];
    double win[BLOCK], wx[BLOCK];
};

/* Work memory for one encode: a struct work per channel signal (four for
 * stereo: left, right, side and mid) and the side and mid signals. */
struct encoder {
    struct work *wk;
    int64_t side[BLOCK], mid[BLOCK];
};

/* ---- residual coding ---- */

static unsigned signed_width(int64_t v)
{
    uint64_t m = v < 0 ? ~(uint64_t)v : (uint64_t)v;
    unsigned w = 1;
    while (m) {
        m >>= 1;
        w++;
    }
    return w;
}

/* The cheapest coding of r[order..n) with the given method's limits.
 * Returns the bits, or UINT64_MAX when a value does not fit in 32 bits. */
static uint64_t code_residual(const int64_t *r, unsigned n, unsigned order, struct coding *best)
{
    for (unsigned i = order; i < n; i++)
        if (r[i] > INT32_MAX || r[i] < INT32_MIN)
            return UINT64_MAX;
    best->bits = UINT64_MAX;
    for (unsigned method = 0; method < 2; method++) {
        unsigned pbits = method ? 5 : 4, kmax = method ? 30 : 14;
        for (unsigned po = 0; po <= MAX_PORDER; po++) {
            unsigned psize = n >> po;
            if ((psize << po) != n || psize < order || (po && psize < 1))
                break;
            struct coding c = { method, po, { 0 }, 2 + 4 };
            unsigned at = order;
            for (unsigned p = 0; p < 1u << po; p++) {
                unsigned cnt = p ? psize : psize - order;
                uint64_t sum = 0;
                unsigned width = 0;
                for (unsigned i = at; i < at + cnt; i++) {
                    uint64_t u = r[i] < 0 ? ((uint64_t)(-r[i]) << 1) - 1 : (uint64_t)r[i] << 1;
                    sum += u;
                    unsigned wdt = r[i] ? signed_width(r[i]) : 0;
                    if (wdt > width)
                        width = wdt;
                }
                /* The parameter near log2 of the mean, then its neighbours. */
                unsigned k0 = 0;
                while (k0 < kmax && (uint64_t)cnt << (k0 + 1) <= sum)
                    k0++;
                unsigned lo = k0 ? k0 - 1 : 0, hi = k0 + 1 <= kmax ? k0 + 1 : kmax;
                uint64_t costs[3] = { 0, 0, 0 };
                for (unsigned i = at; i < at + cnt; i++) {
                    uint64_t u = r[i] < 0 ? ((uint64_t)(-r[i]) << 1) - 1 : (uint64_t)r[i] << 1;
                    for (unsigned k = lo; k <= hi; k++)
                        costs[k - lo] += u >> k;
                }
                uint64_t pick = UINT64_MAX;
                unsigned param = 0;
                for (unsigned k = lo; k <= hi; k++) {
                    uint64_t bits = costs[k - lo] + (uint64_t)cnt * (k + 1);
                    if (bits < pick) {
                        pick = bits;
                        param = k;
                    }
                }
                uint64_t escaped = 5 + (uint64_t)cnt * width;
                if (width < 32 && escaped < pick) {
                    pick = escaped;
                    param = ESCAPED | width;
                }
                c.param[p] = param;
                c.bits += pbits + pick;
                at += cnt;
            }
            if (c.bits < best->bits)
                *best = c;
        }
    }
    return best->bits;
}

/* ---- predictors ---- */

static void fixed_residual(const int64_t *x, unsigned n, unsigned order, int64_t *r)
{
    for (unsigned i = order; i < n; i++) {
        switch (order) {
        case 0: r[i] = x[i]; break;
        case 1: r[i] = x[i] - x[i - 1]; break;
        case 2: r[i] = x[i] - 2 * x[i - 1] + x[i - 2]; break;
        case 3: r[i] = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3]; break;
        default: r[i] = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4]; break;
        }
    }
}

/* Tukey window with half of the block in its cosine tapers. */
static void tukey(double *w, unsigned n)
{
    double taper = 0.25 * (n - 1);
    for (unsigned i = 0; i < n; i++) {
        double d = i < n / 2 ? i : n - 1 - i;
        w[i] = d < taper ? 0.5 * (1 - cos(M_PI * d / taper)) : 1.0;
    }
}

/* Quantise the coefficients c[0..order) to precision bits with error
 * feedback. Returns the shift, or -1 when no shift from 0 to 15 fits. */
static int quantise(const double *c, unsigned order, unsigned precision, int32_t *q)
{
    double cmax = 0;
    for (unsigned j = 0; j < order; j++)
        if (fabs(c[j]) > cmax)
            cmax = fabs(c[j]);
    if (cmax <= 0)
        return -1;
    int exponent;
    frexp(cmax, &exponent);
    int shift = (int)precision - 1 - exponent;
    if (shift > 15)
        shift = 15;
    if (shift < 0)
        return -1;
    int32_t lim = (1 << (precision - 1)) - 1;
    double carry = 0;
    for (unsigned j = 0; j < order; j++) {
        carry += c[j] * (double)(1 << shift);
        long v = (long)(carry >= 0 ? carry + 0.5 : carry - 0.5);
        if (v > lim)
            v = lim;
        if (v < -lim - 1)
            v = -lim - 1;
        q[j] = (int32_t)v;
        carry -= (double)v;
    }
    return shift;
}

static void lpc_residual(const int64_t *x, unsigned n, unsigned order, const int32_t *q, int shift, int64_t *r)
{
    for (unsigned i = order; i < n; i++) {
        int64_t sum = 0;
        for (unsigned j = 0; j < order; j++)
            sum += (int64_t)q[j] * x[i - 1 - j];
        r[i] = x[i] - (sum >> shift);
    }
}

/* ---- choosing a subframe ---- */

static uint64_t header_bits(unsigned wasted)
{
    return 8 + wasted;                  /* type and flag, then the unary count */
}

static void choose(struct work *wk, const int64_t *signal, unsigned n, unsigned bits, struct subframe *best)
{
    uint64_t any = 0;
    for (unsigned i = 0; i < n; i++)
        any |= (uint64_t)signal[i];
    unsigned wasted = any ? (unsigned)__builtin_ctzll(any) : 0;
    if (wasted >= bits)
        wasted = 0;
    unsigned eb = bits - wasted;
    for (unsigned i = 0; i < n; i++)
        wk->x[i] = signal[i] >> wasted;
    best->x = wk->x;
    best->res = wk->res;
    best->bits = eb;
    best->wasted = wasted;

    int constant = 1;
    for (unsigned i = 1; i < n && constant; i++)
        constant = wk->x[i] == wk->x[0];
    if (constant) {
        best->type = SUB_CONSTANT;
        best->cost = header_bits(wasted) + eb;
        return;
    }
    best->type = SUB_VERBATIM;
    best->cost = header_bits(wasted) + (uint64_t)n * eb;

    /* FIXED: the order with the smallest sum of absolute residuals. */
    unsigned fo = 0;
    uint64_t fsum = UINT64_MAX;
    for (unsigned order = 0; order <= 4 && order < n; order++) {
        fixed_residual(wk->x, n, order, wk->trial);
        uint64_t sum = 0;
        for (unsigned i = order; i < n; i++)
            sum += (uint64_t)(wk->trial[i] < 0 ? -wk->trial[i] : wk->trial[i]);
        if (sum < fsum) {
            fsum = sum;
            fo = order;
        }
    }
    struct coding c;
    fixed_residual(wk->x, n, fo, wk->trial);
    uint64_t cost = code_residual(wk->trial, n, fo, &c);
    if (cost != UINT64_MAX && header_bits(wasted) + (uint64_t)fo * eb + cost < best->cost) {
        best->type = SUB_FIXED;
        best->order = fo;
        best->coding = c;
        best->cost = header_bits(wasted) + (uint64_t)fo * eb + cost;
        memcpy(wk->res, wk->trial, n * sizeof *wk->res);
    }

    /* LPC: the Levinson-Durbin recursion over the windowed autocorrelation
     * gives the coefficients and the prediction error of every order.
     * The orders with the smallest estimated size are coded exactly at
     * two coefficient precisions. */
    unsigned maxorder = n > FLAC_MAX_ORDER ? FLAC_MAX_ORDER : n - 1;
    if (n < 16 || maxorder < 1)
        return;
    tukey(wk->win, n);
    for (unsigned i = 0; i < n; i++)
        wk->wx[i] = (double)wk->x[i] * wk->win[i];
    double autoc[FLAC_MAX_ORDER + 1];
    for (unsigned l = 0; l <= maxorder; l++) {
        double s = 0;
        for (unsigned i = l; i < n; i++)
            s += wk->wx[i] * wk->wx[i - l];
        autoc[l] = s;
    }
    if (autoc[0] <= 0)
        return;
    autoc[0] *= 1.0 + 1e-9;             /* ensures the recursion remains stable on pure tones */
    double a[FLAC_MAX_ORDER + 1] = { 1 }, coef[FLAC_MAX_ORDER][FLAC_MAX_ORDER], err[FLAC_MAX_ORDER];
    double e = autoc[0];
    unsigned orders = 0;
    for (unsigned i = 1; i <= maxorder; i++) {
        double acc = autoc[i];
        for (unsigned j = 1; j < i; j++)
            acc += a[j] * autoc[i - j];
        double k = -acc / e;
        double prev[FLAC_MAX_ORDER + 1];
        memcpy(prev, a, sizeof prev);
        for (unsigned j = 1; j < i; j++)
            a[j] = prev[j] + k * prev[i - j];
        a[i] = k;
        e *= 1 - k * k;
        if (!(e > 0))
            break;
        for (unsigned j = 0; j < i; j++)
            coef[i - 1][j] = -a[j + 1];
        err[i - 1] = e;
        orders = i;
    }
    /* Estimated bits: the entropy of a Laplacian residual of the error
     * variance, plus the warm-up samples and the coefficients. */
    unsigned try_order[LPC_TRIES] = { 0 };
    double try_bits[LPC_TRIES];
    for (unsigned t = 0; t < LPC_TRIES; t++)
        try_bits[t] = INFINITY;
    for (unsigned o = 1; o <= orders; o++) {
        double var = err[o - 1] / autoc[0] * (autoc[0] / n);
        double per = var > 1 ? 0.5 * log2(var) + 1.0 : 1.0;
        double est = per * (n - o) + (double)o * (eb + 15);
        for (unsigned t = 0; t < LPC_TRIES; t++)
            if (est < try_bits[t]) {
                memmove(try_bits + t + 1, try_bits + t, (LPC_TRIES - 1 - t) * sizeof try_bits[0]);
                memmove(try_order + t + 1, try_order + t, (LPC_TRIES - 1 - t) * sizeof try_order[0]);
                try_bits[t] = est;
                try_order[t] = o;
                break;
            }
    }
    static const unsigned precisions[] = { 12, 15 };
    for (unsigned t = 0; t < LPC_TRIES; t++) {
        unsigned order = try_order[t];
        if (!order)
            continue;
        for (unsigned pi = 0; pi < 2; pi++) {
            unsigned precision = precisions[pi];
            int32_t q[FLAC_MAX_ORDER];
            int shift = quantise(coef[order - 1], order, precision, q);
            if (shift < 0)
                continue;
            lpc_residual(wk->x, n, order, q, shift, wk->trial);
            cost = code_residual(wk->trial, n, order, &c);
            if (cost == UINT64_MAX)
                continue;
            uint64_t total = header_bits(wasted) + (uint64_t)order * eb + 4 + 5 + (uint64_t)order * precision + cost;
            if (total < best->cost) {
                best->type = SUB_LPC;
                best->order = order;
                best->precision = precision;
                best->shift = shift;
                memcpy(best->coef, q, order * sizeof q[0]);
                best->coding = c;
                best->cost = total;
                memcpy(wk->res, wk->trial, n * sizeof *wk->res);
            }
        }
    }
}

/* ---- writing ---- */

static void write_residual(struct flac_writer *w, const int64_t *r, unsigned n, unsigned order,
                           const struct coding *c)
{
    unsigned pbits = c->method ? 5 : 4, escape = c->method ? 31 : 15;
    flac_put(w, c->method, 2);
    flac_put(w, c->porder, 4);
    unsigned psize = n >> c->porder, at = order;
    for (unsigned p = 0; p < 1u << c->porder; p++) {
        unsigned cnt = p ? psize : psize - order, param = c->param[p];
        if (param & ESCAPED) {
            unsigned width = param & 0xff;
            flac_put(w, escape, pbits);
            flac_put(w, width, 5);
            for (unsigned i = at; i < at + cnt && width; i++)
                flac_put_signed(w, r[i], width);
        } else {
            flac_put(w, param, pbits);
            for (unsigned i = at; i < at + cnt; i++) {
                uint64_t u = r[i] < 0 ? ((uint64_t)(-r[i]) << 1) - 1 : (uint64_t)r[i] << 1;
                flac_put_unary(w, (uint32_t)(u >> param));
                flac_put(w, u, param);
            }
        }
        at += cnt;
    }
}

static void write_subframe(struct flac_writer *w, const struct subframe *s, unsigned n)
{
    static const unsigned codes[] = { 0, 1, 8, 31 };
    unsigned type = codes[s->type] + (s->type == SUB_FIXED || s->type == SUB_LPC ? s->order : 0);
    flac_put(w, 0, 1);
    flac_put(w, type, 6);
    flac_put(w, s->wasted != 0, 1);
    if (s->wasted)
        flac_put_unary(w, s->wasted - 1);
    switch (s->type) {
    case SUB_CONSTANT:
        flac_put_signed(w, s->x[0], s->bits);
        break;
    case SUB_VERBATIM:
        for (unsigned i = 0; i < n; i++)
            flac_put_signed(w, s->x[i], s->bits);
        break;
    case SUB_FIXED:
        for (unsigned i = 0; i < s->order; i++)
            flac_put_signed(w, s->x[i], s->bits);
        write_residual(w, s->res, n, s->order, &s->coding);
        break;
    default:
        for (unsigned i = 0; i < s->order; i++)
            flac_put_signed(w, s->x[i], s->bits);
        flac_put(w, s->precision - 1, 4);
        flac_put_signed(w, s->shift, 5);
        for (unsigned j = 0; j < s->order; j++)
            flac_put_signed(w, s->coef[j], s->precision);
        write_residual(w, s->res, n, s->order, &s->coding);
        break;
    }
}

static void put_number(struct flac_writer *w, uint64_t v)
{
    if (v < 0x80) {
        flac_put(w, v, 8);
        return;
    }
    unsigned extra = 1;
    while (extra < 6 && v >= 1ull << (6 * extra + 6 - extra))
        extra++;
    flac_put(w, (0xff00u >> (extra + 1) & 0xff) | (unsigned)(v >> (6 * extra)), 8);
    for (unsigned i = extra; i-- > 0;)
        flac_put(w, 0x80 | ((v >> (6 * i)) & 0x3f), 8);
}

static unsigned rate_code(unsigned rate)
{
    for (unsigned c = 1; c < 12; c++)
        if (flac_rates[c] == rate)
            return c;
    if (rate % 1000 == 0 && rate / 1000 < 256)
        return 12;
    if (rate < 65536)
        return 13;
    if (rate % 10 == 0 && rate / 10 < 65536)
        return 14;
    return 0;
}

static unsigned size_code(unsigned bits)
{
    for (unsigned c = 1; c < 8; c++)
        if (flac_sample_sizes[c] == bits)
            return c;
    return 0;
}

/* One frame of n samples per channel, from ch[c][0..n). */
static int write_frame(struct flac_writer *w, struct encoder *enc, int64_t *const *ch, unsigned channels,
                       unsigned n, unsigned bits, unsigned rate, uint64_t number)
{
    struct subframe sub[FLAC_MAX_CHANNELS];
    struct work *wk = enc->wk;
    unsigned assignment = channels - 1;
    if (channels == 2) {
        /* The four signals of a stereo frame, each in its own work memory:
         * left, right, side (one bit wider) and mid. */
        int64_t *side = enc->side, *mid = enc->mid;
        for (unsigned i = 0; i < n; i++) {
            side[i] = ch[0][i] - ch[1][i];
            mid[i] = (ch[0][i] + ch[1][i]) >> 1;
        }
        struct subframe cand[4];
        for (unsigned k = 0; k < 4; k++) {
            const int64_t *sig = k == 0 ? ch[0] : k == 1 ? ch[1] : k == 2 ? side : mid;
            choose(&wk[k], sig, n, bits + (k == 2), &cand[k]);
        }
        uint64_t costs[4] = { cand[0].cost + cand[1].cost, cand[0].cost + cand[2].cost,
                              cand[2].cost + cand[1].cost, cand[3].cost + cand[2].cost };
        unsigned pick = 0;
        for (unsigned k = 1; k < 4; k++)
            if (costs[k] < costs[pick])
                pick = k;
        static const unsigned firsts[4] = { 0, 0, 2, 3 }, seconds[4] = { 1, 2, 1, 2 };
        sub[0] = cand[firsts[pick]];
        sub[1] = cand[seconds[pick]];
        assignment = pick ? FLAC_LEFT_SIDE + pick - 1 : 1;
    } else {
        for (unsigned c = 0; c < channels; c++)
            choose(&wk[c], ch[c], n, bits, &sub[c]);
    }
    size_t start = w->len;
    flac_put(w, 0xfff8, 16);
    unsigned bs = 0;
    for (unsigned c = 1; c < 16; c++)
        if (flac_block_sizes[c] == n)
            bs = c;
    if (!bs)
        bs = n <= 256 ? 6 : 7;
    unsigned rc = rate_code(rate);
    flac_put(w, bs, 4);
    flac_put(w, rc, 4);
    flac_put(w, assignment, 4);
    flac_put(w, size_code(bits), 3);
    flac_put(w, 0, 1);
    put_number(w, number);
    if (bs == 6)
        flac_put(w, n - 1, 8);
    else if (bs == 7)
        flac_put(w, n - 1, 16);
    if (rc == 12)
        flac_put(w, rate / 1000, 8);
    else if (rc == 13)
        flac_put(w, rate, 16);
    else if (rc == 14)
        flac_put(w, rate / 10, 16);
    if (w->failed)
        return -ENOMEM;
    flac_put(w, flac_crc8(w->data + start, w->len - start), 8);
    for (unsigned c = 0; c < channels; c++)
        write_subframe(w, &sub[c], n);
    flac_put_align(w);
    if (w->failed)
        return -ENOMEM;
    flac_put(w, flac_crc16(w->data + start, w->len - start), 16);
    return w->failed ? -ENOMEM : 0;
}

int flac_encode_stream(const struct codec_audio_format *fmt, const int32_t *samples, long frames,
                       struct flac_encoded *out)
{
    unsigned bits = fmt->bits ? (unsigned)fmt->bits : 16, channels = (unsigned)fmt->channels;
    memset(out, 0, sizeof *out);
    if (bits < 4 || bits > 32 || channels < 1 || channels > FLAC_MAX_CHANNELS || fmt->rate <= 0 ||
        fmt->rate >= 1 << 20 || (uint64_t)frames >= 1ull << 36)
        return -EINVAL;
    long count = (frames + BLOCK - 1) / BLOCK;
    struct encoder *enc = calloc(1, sizeof *enc);
    if (enc)
        enc->wk = calloc(channels == 2 ? 4 : channels, sizeof *enc->wk);
    int64_t *ch[FLAC_MAX_CHANNELS] = { NULL };
    for (unsigned c = 0; c < channels; c++)
        ch[c] = malloc(BLOCK * sizeof *ch[c]);
    out->ends = malloc(sizeof *out->ends * (size_t)(count ? count : 1));
    out->samples = malloc(sizeof *out->samples * (size_t)(count ? count : 1));
    struct flac_writer w = { 0 };
    struct codec_md5 md5;
    codec_md5_init(&md5);
    int err = enc && enc->wk && out->ends && out->samples ? 0 : -ENOMEM;
    for (unsigned c = 0; c < channels && !err; c++)
        if (!ch[c])
            err = -ENOMEM;
    uint32_t min_frame = UINT32_MAX, max_frame = 0;
    unsigned shift = 32 - bits;
    for (long at = 0, number = 0; at < frames && !err; at += BLOCK, number++) {
        unsigned n = frames - at < BLOCK ? (unsigned)(frames - at) : BLOCK;
        for (unsigned i = 0; i < n; i++)
            for (unsigned c = 0; c < channels; c++)
                ch[c][i] = (int64_t)samples[(size_t)(at + i) * channels + c] >> shift;
        flac_md5_samples(&md5, (const int64_t *const *)ch, channels, n, bits);
        size_t before = w.len;
        err = write_frame(&w, enc, ch, channels, n, bits, (unsigned)fmt->rate, (uint64_t)number);
        uint32_t size = (uint32_t)(w.len - before);
        if (size < min_frame)
            min_frame = size;
        if (size > max_frame)
            max_frame = size;
        out->ends[number] = w.len;
        out->samples[number] = n;
        out->count = number + 1;
    }
    if (enc)
        free(enc->wk);
    free(enc);
    for (unsigned c = 0; c < channels; c++)
        free(ch[c]);
    if (err) {
        free(w.data);
        flac_encoded_free(out);
        return err;
    }
    /* STREAMINFO: the block size of the frames, the frame size limits,
     * the format, the sample count and the MD5 sum. */
    struct flac_writer h = { 0 };
    unsigned block = frames >= BLOCK ? BLOCK : frames < 16 ? 16 : (unsigned)frames;
    uint8_t sum[16];
    codec_md5_final(&md5, sum);
    flac_put(&h, block, 16);
    flac_put(&h, block, 16);
    flac_put(&h, frames ? min_frame : 0, 24);
    flac_put(&h, max_frame, 24);
    flac_put(&h, (unsigned)fmt->rate, 20);
    flac_put(&h, channels - 1, 3);
    flac_put(&h, bits - 1, 5);
    flac_put(&h, (uint64_t)frames, 36);
    for (int i = 0; i < 16; i++)
        flac_put(&h, sum[i], 8);
    if (h.failed || h.len != FLAC_STREAMINFO_LEN) {
        free(h.data);
        free(w.data);
        flac_encoded_free(out);
        return -ENOMEM;
    }
    memcpy(out->streaminfo, h.data, FLAC_STREAMINFO_LEN);
    free(h.data);
    out->frames = w.data;
    out->len = w.len;
    return 0;
}

void flac_encoded_free(struct flac_encoded *e)
{
    free(e->frames);
    free(e->ends);
    free(e->samples);
    memset(e, 0, sizeof *e);
}

/* A native stream: the marker, STREAMINFO as the only and last metadata
 * block, and the frames. */
long flac_encode(const struct codec_audio_format *fmt, const int32_t *samples, long frames, uint8_t **result)
{
    struct flac_encoded e;
    int err = flac_encode_stream(fmt, samples, frames, &e);
    if (err)
        return err;
    size_t total = 8 + FLAC_STREAMINFO_LEN + e.len;
    uint8_t *out = malloc(total);
    if (!out) {
        flac_encoded_free(&e);
        return -ENOMEM;
    }
    memcpy(out, "fLaC", 4);
    out[4] = 0x80;                      /* the last metadata block, STREAMINFO */
    out[5] = 0;
    out[6] = 0;
    out[7] = FLAC_STREAMINFO_LEN;
    memcpy(out + 8, e.streaminfo, FLAC_STREAMINFO_LEN);
    if (e.len)
        memcpy(out + 8 + FLAC_STREAMINFO_LEN, e.frames, e.len);
    flac_encoded_free(&e);
    *result = out;
    return (long)total;
}
