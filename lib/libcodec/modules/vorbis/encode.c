/* Vorbis encoding. The encoder works in two passes over the whole input.
 * The first pass cuts the signal into blocks of 256 or 2048 samples,
 * computes for every block and channel a floor and the quantised residue,
 * couples stereo channels, classifies the residue partitions and counts
 * the symbols of every codebook. Huffman codes are then built from those
 * counts, and the second pass writes the three headers and one packet per
 * block into Ogg pages.
 *
 * The floor is the masking estimate of the block: the spectral envelope
 * lowered by a signal-to-noise ratio set by the quality. The residue is
 * the spectrum divided by the floor as the decoder renders it, rounded to
 * integers, which makes the floor the quantisation step. Options:
 * quality=Q from -0.1 to 1.0 (0.4 by default) and floor=0 or floor=1. */
#include "vorbis.h"
#include <math.h>

#define SHORT 256
#define LONG 2048
#define PARTITION 32
#define CLASSES 6
#define MAX_RESIDUE 4095                /* a residue value before coupling */
#define MAX_COUPLED 8190                /* the angle of two coupled residues */
#define FLOOR0_BARK 256
#define FLOOR0_AMP_BITS 8
#define FLOOR0_OFFSET 140

/* The codebooks of the setup header. */
enum { BOOK_Y, BOOK_CLASS, BOOK_C1, BOOK_C2, BOOK_C3, BOOK_COARSE, BOOK_FINE, BOOK_LSP, BOOK_HUGE, BOOK_MID,
       NBOOKS };

/* The vector books: dimensions, grid size, smallest value and step. */
static const struct {
    unsigned dims, values;
    int min, delta;
} grids[NBOOKS] = {
    [BOOK_C1] = { 4, 3, -1, 1 },      [BOOK_C2] = { 2, 9, -4, 1 },   [BOOK_C3] = { 2, 31, -15, 1 },
    [BOOK_COARSE] = { 2, 63, -496, 16 }, [BOOK_FINE] = { 2, 17, -8, 1 }, [BOOK_HUGE] = { 2, 65, -8192, 256 },
    [BOOK_MID] = { 2, 17, -128, 16 },
};
#define LSP_ENTRIES 256

/* Rounding to the nearest integer, halves away from zero. */
static long rnd(double v)
{
    return (long)(v < 0 ? v - 0.5 : v + 0.5);
}

/* ---- the bit writer, least significant bits first ---- */

struct vb_writer {
    uint8_t *data;
    size_t len, cap;                    /* bytes */
    unsigned bit;                       /* bits used in the last byte */
    int failed;
};

static void put(struct vb_writer *w, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (w->bit == 0) {
            if (w->len == w->cap) {
                size_t cap = w->cap ? w->cap * 2 : 4096;
                uint8_t *g = realloc(w->data, cap);
                if (!g) {
                    w->failed = 1;
                    return;
                }
                w->data = g;
                w->cap = cap;
            }
            w->data[w->len++] = 0;
        }
        w->data[w->len - 1] |= (uint8_t)(((v >> i) & 1) << w->bit);
        w->bit = (w->bit + 1) & 7;
    }
}

static void put_bytes(struct vb_writer *w, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        put(w, (uint8_t)s[i], 8);
}

/* The packed float of the codebooks for a value that is a multiple of a
 * power of two with a 21 bit mantissa. */
static uint32_t pack_float(double v)
{
    uint32_t sign = v < 0 ? 0x80000000u : 0;
    v = fabs(v);
    if (v == 0)
        return 0;
    int exp = 788;
    while (v >= (1 << 21)) {
        v /= 2;
        exp++;
    }
    while (v < (1 << 20)) {
        v *= 2;
        exp--;
    }
    uint32_t mantissa = (uint32_t)rnd(v);
    if (mantissa >= 1u << 21) {
        mantissa >>= 1;
        exp++;
    }
    return sign | (uint32_t)exp << 21 | mantissa;
}

/* ---- Huffman codes ---- */

struct code {
    unsigned entries;
    uint32_t *count;
    uint8_t *len;
    uint32_t *word;                     /* most significant bit first */
};

/* Code lengths from counts by the Huffman algorithm, longest 32 bits. An
 * entry without a count gets no codeword, and a book with fewer than two
 * used entries receives one more, as single entry books are read
 * differently by some decoders. */
static int huffman(struct code *c)
{
    unsigned n = c->entries, used = 0;
    for (unsigned i = 0; i < n; i++)
        used += c->count[i] != 0;
    for (unsigned i = 0; used < 2 && i < n; i++)
        if (!c->count[i]) {
            c->count[i] = 1;
            used++;
        }
    uint64_t *weight = malloc(sizeof *weight * 2 * n);
    int *parent = malloc(sizeof *parent * 2 * n), *alive = malloc(sizeof *alive * 2 * n);
    if (!weight || !parent || !alive) {
        free(weight);
        free(parent);
        free(alive);
        return -ENOMEM;
    }
    for (int scale = 0;; scale++) {
        unsigned nodes = n;
        for (unsigned i = 0; i < n; i++) {
            weight[i] = c->count[i] ? (c->count[i] >> scale) + 1 : 0;
            alive[i] = c->count[i] != 0;
            parent[i] = -1;
        }
        for (unsigned remaining = used; remaining > 1; remaining--) {
            int a = -1, b = -1;
            for (unsigned i = 0; i < nodes; i++) {
                if (!alive[i])
                    continue;
                if (a < 0 || weight[i] < weight[a]) {
                    b = a;
                    a = (int)i;
                } else if (b < 0 || weight[i] < weight[b]) {
                    b = (int)i;
                }
            }
            weight[nodes] = weight[a] + weight[b];
            alive[nodes] = 1;
            parent[nodes] = -1;
            alive[a] = alive[b] = 0;
            parent[a] = parent[b] = (int)nodes;
            nodes++;
        }
        unsigned longest = 0;
        for (unsigned i = 0; i < n; i++) {
            unsigned depth = 0;
            if (c->count[i])
                for (int p = parent[i]; p >= 0; p = parent[p])
                    depth++;
            c->len[i] = (uint8_t)depth;
            if (depth > longest)
                longest = depth;
        }
        if (longest <= 32)
            break;
    }
    free(weight);
    free(parent);
    free(alive);
    /* The codewords in the order the decoder assigns them. */
    uint32_t available[33] = { 0 };
    int first = 1;
    for (unsigned e = 0; e < n; e++) {
        unsigned len = c->len[e];
        if (!len)
            continue;
        if (first) {
            first = 0;
            c->word[e] = 0;
            for (unsigned i = 1; i <= len; i++)
                available[i] = 1u << (32 - i);
            continue;
        }
        unsigned z = len;
        while (z > 0 && !available[z])
            z--;
        uint32_t res = available[z];
        available[z] = 0;
        for (unsigned y = len; y > z; y--)
            available[y] = res + (1u << (32 - y));
        c->word[e] = res >> (32 - len);
    }
    return 0;
}

static void put_code(struct vb_writer *w, const struct code *c, unsigned entry)
{
    unsigned len = c->len[entry];
    for (unsigned i = 0; i < len; i++)
        put(w, (c->word[entry] >> (len - 1 - i)) & 1, 1);
}

/* ---- the encoder ---- */

struct block {
    unsigned flag, prevflag, nextflag;
    long center;
    uint8_t *unused;                    /* per channel */
    uint8_t *y;                         /* floor 1: per channel the two end points and the post values */
    uint16_t *lsp;                      /* floor 0: per channel the amplitude, then the increments */
    int16_t *res;                       /* the residue after coupling, in the order of the residue type */
    uint8_t *cls;                       /* the classification of every partition */
};

struct encoder {
    unsigned channels, rate, floor_type;
    double quality;
    struct vb_setup setup;              /* floors and codebooks as the decoder sees them */
    struct codec_mdct mdct[2];
    float *ramp[2];
    struct code books[NBOOKS];
    struct block *blocks;
    long nblocks;
    unsigned posts[2];                  /* floor 1 values per block size, the end points included */
    unsigned order[2];                  /* floor 0 order per block size */
};

static unsigned book_entries(unsigned b)
{
    if (b == BOOK_Y)
        return 128;
    if (b == BOOK_CLASS)
        return CLASSES * CLASSES;
    if (b == BOOK_LSP)
        return LSP_ENTRIES;
    unsigned e = 1;
    for (unsigned i = 0; i < grids[b].dims; i++)
        e *= grids[b].values;
    return e;
}

/* The floor 1 positions of a block size: 0, the end, and the others
 * spaced geometrically, in an order that halves the intervals, which
 * ensures that the predictions of the decoder remain close. */
static void floor1_layout(struct vb_floor1 *f, unsigned half, unsigned count)
{
    unsigned sorted[VB_MAX_FLOOR1_VALUES], n = 0;
    for (unsigned i = 0; n < count && i < 4 * count; i++) {
        double t = (double)(i + 1) / (count + 1);
        unsigned x = (unsigned)rnd(2 * pow((double)half / 2, t));
        if (x <= 1 || x >= half)
            continue;
        int dup = 0;
        for (unsigned j = 0; j < n; j++)
            dup |= sorted[j] == x;
        if (!dup)
            sorted[n++] = x;
    }
    for (unsigned x = 2; n < count && x < half; x++) {
        int dup = 0;
        for (unsigned j = 0; j < n; j++)
            dup |= sorted[j] == x;
        if (!dup)
            sorted[n++] = x;
    }
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && sorted[j - 1] > sorted[j]; j--) {
            unsigned t = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = t;
        }
    n -= n % 4;                         /* whole partitions of four posts */
    /* Breadth first over the halves of the sorted list. */
    unsigned order[VB_MAX_FLOOR1_VALUES], m = 0, queue[2 * VB_MAX_FLOOR1_VALUES][2], qh = 0, qt = 0;
    queue[qt][0] = 0;
    queue[qt++][1] = n;
    while (qh < qt) {
        unsigned lo = queue[qh][0], hi = queue[qh++][1];
        if (lo >= hi)
            continue;
        unsigned mid = (lo + hi) / 2;
        order[m++] = sorted[mid];
        queue[qt][0] = lo;
        queue[qt++][1] = mid;
        queue[qt][0] = mid + 1;
        queue[qt++][1] = hi;
    }
    f->multiplier = 2;
    f->partitions = n / 4;
    f->classes = 1;
    f->class_dims[0] = 4;
    f->class_subclasses[0] = 0;
    f->subclass_books[0][0] = BOOK_Y;
    for (unsigned i = 0; i < f->partitions; i++)
        f->partition_class[i] = 0;
    f->x[0] = 0;
    f->x[1] = (uint16_t)half;
    f->values = 2 + n;
    for (unsigned i = 0; i < n; i++)
        f->x[2 + i] = (uint16_t)order[i];
}

/* The neighbour tables of a floor 1, as setup.c computes them. */
static void floor1_neighbours(struct vb_floor1 *g)
{
    for (unsigned i = 0; i < g->values; i++)
        g->sorted[i] = (uint8_t)i;
    for (unsigned i = 1; i < g->values; i++)
        for (unsigned j = i; j > 0 && g->x[g->sorted[j - 1]] > g->x[g->sorted[j]]; j--) {
            uint8_t t = g->sorted[j];
            g->sorted[j] = g->sorted[j - 1];
            g->sorted[j - 1] = t;
        }
    for (unsigned i = 2; i < g->values; i++) {
        int lo = -1, hi = -1;
        for (unsigned j = 0; j < i; j++) {
            if (g->x[j] < g->x[i] && (lo < 0 || g->x[j] > g->x[lo]))
                lo = (int)j;
            if (g->x[j] > g->x[i] && (hi < 0 || g->x[j] < g->x[hi]))
                hi = (int)j;
        }
        g->low[i] = (uint8_t)lo;
        g->high[i] = (uint8_t)hi;
    }
}

/* The value that makes the decoder's floor 1 prediction pred become
 * want (section 7.2.4), or 0 to retain the prediction. */
static int post_value(int pred, int want, int range)
{
    int highroom = range - pred, lowroom = pred, room = (highroom < lowroom ? highroom : lowroom) * 2;
    int diff = want - pred;
    if (diff == 0)
        return 0;
    if (diff > 0)
        return 2 * diff < room ? 2 * diff : diff + lowroom;
    int d = -diff;
    return 2 * d - 1 < room ? 2 * d - 1 : d + highroom - 1;
}

static int render_point(int x0, int y0, int x1, int y1, int x)
{
    int dy = y1 - y0, adx = x1 - x0, ady = dy < 0 ? -dy : dy;
    int off = ady * (x - x0) / adx;
    return dy < 0 ? y0 - off : y0 + off;
}

/* The masking estimate of a block for the band [lo, hi) around a
 * position, as a floor 1 table index from 0 to 255. The estimate is the
 * largest magnitude in the band lowered by the signal-to-noise ratio of
 * the quality. A noise-like band, recognised by the spectral flatness of
 * its power, masks more of its own quantisation noise and needs up to
 * 18 dB less, and above 8 kHz the ratio falls by up to 6 dB. Both
 * allowances shrink as the quality rises and vanish at 1.0. The estimate
 * never falls below the absolute threshold of hearing (Terhardt's
 * approximation, with full scale taken as 96 dB SPL), which is lowered
 * by up to 30 dB at the highest quality and capped at 90 - 70 * quality
 * dB SPL, retaining the high frequencies at high qualities. */
static double threshold_index(const struct encoder *e, const float *spec, unsigned half, unsigned lo, unsigned hi,
                              unsigned center)
{
    double peak = 0, sum = 0, logs = 0;
    unsigned count = 0;
    for (unsigned k = lo; k < hi && k < half; k++) {
        double p = (double)spec[k] * spec[k] + 1e-20;
        peak = fmax(peak, fabs(spec[k]));
        sum += p;
        logs += log(p);
        count++;
    }
    double flatness = count ? exp(logs / count) / (sum / count) : 0;
    double freq = fmax((double)center * e->rate / (2.0 * half), 20);
    double relax = 1 - fmin(1, fmax(0, e->quality));
    double snr = 12 + 36 * e->quality -
                 relax * (18 * flatness + (freq > 8000 ? 6 * fmin(1, (freq - 8000) / 8000) : 0));
    double khz = freq / 1000;
    double ath = 3.64 * pow(khz, -0.8) - 6.5 * exp(-0.6 * (khz - 3.3) * (khz - 3.3)) + 0.001 * pow(khz, 4);
    ath = fmin(ath, 90 - 70 * e->quality);  /* the steep rise above 16 kHz, capped */
    double amp = peak * pow(10, -snr / 20), floor_amp = pow(10, (ath - 96 - 30 * e->quality) / 20);
    if (amp < floor_amp)
        amp = floor_amp;
    double index = 255 - 255 * log(amp) / log(1.0649863e-07);
    return index < 0 ? 0 : index > 255 ? 255 : index;
}

/* Floor 1 for one channel of a block: the desired values at the posts,
 * coded against the predictions of the decoder. */
static void floor1_encode(const struct encoder *e, const struct vb_floor1 *f, const float *spec, unsigned half,
                          int lift, uint8_t *y)
{
    unsigned sorted_x[VB_MAX_FLOOR1_VALUES];
    for (unsigned i = 0; i < f->values; i++)
        sorted_x[i] = f->x[f->sorted[i]];
    int want[VB_MAX_FLOOR1_VALUES];
    for (unsigned k = 0; k < f->values; k++) {
        unsigned x = sorted_x[k];
        unsigned lo = k ? (sorted_x[k - 1] + x) / 2 : 0, hi = k + 1 < f->values ? (x + sorted_x[k + 1] + 1) / 2 : half;
        want[f->sorted[k]] = (int)rnd(threshold_index(e, spec, half, lo, hi > lo ? hi : lo + 1, x) / 2) + lift;
        if (want[f->sorted[k]] > 127)
            want[f->sorted[k]] = 127;
    }
    int final[VB_MAX_FLOOR1_VALUES];
    final[0] = want[0];
    final[1] = want[1];
    y[0] = (uint8_t)want[0];
    y[1] = (uint8_t)want[1];
    for (unsigned i = 2; i < f->values; i++) {
        unsigned lo = f->low[i], hi = f->high[i];
        int pred = render_point(f->x[lo], final[lo], f->x[hi], final[hi], f->x[i]);
        int v = post_value(pred, want[i], 128);
        y[i] = (uint8_t)v;
        /* Apply the decoder's rule to learn the final value. */
        int highroom = 128 - pred, lowroom = pred, room = (highroom < lowroom ? highroom : lowroom) * 2;
        if (!v)
            final[i] = pred;
        else if (v >= room)
            final[i] = highroom > lowroom ? v - lowroom + pred : pred - v + highroom - 1;
        else
            final[i] = v & 1 ? pred - (v + 1) / 2 : pred + v / 2;
    }
}

/* ---- floor 0 ---- */

/* The sum or difference polynomial of order + 1 coefficients on the unit
 * circle, rotated by half its degree: real for the symmetric P, imaginary
 * for the antisymmetric Q, of which the cosine or sine part is returned.
 * The angles of the terms are advanced by a rotation, which needs one
 * cosine and one sine per evaluation. */
static double poly_value(const double *c, unsigned order, double w, int odd)
{
    double a = -w * (order + 1) / 2.0, ca = cos(a), sa = sin(a), cw = cos(w), sw = sin(w), v = 0;
    for (unsigned i = 0; i <= order + 1; i++) {
        v += c[i] * (odd ? sa : ca);
        double t = ca * cw - sa * sw;
        sa = sa * cw + ca * sw;
        ca = t;
    }
    return v;
}

/* Fit floor 0 to the masking estimate. The target curve in decibels plus
 * the amplitude offset is sampled on the Bark scale of the decoder, an LPC
 * filter is fitted to its square, and the roots of the sum and difference
 * polynomials of the filter give the line spectral pairs. The amplitude is
 * the least squares fit of the decoder's curve to the target. */
static void floor0_encode(const struct encoder *e, const struct vb_floor0 *f, unsigned blockflag, const float *spec,
                          unsigned half, uint16_t *out, const struct vb_floor *floor)
{
    unsigned order = f->order;
    const int32_t *map = f->map[blockflag];
    double *target = malloc(sizeof *target * half), *wsum = calloc(FLOOR0_BARK, sizeof *wsum);
    double *wcount = calloc(FLOOR0_BARK, sizeof *wcount);
    float *curve = malloc(sizeof *curve * half);
    if (!target || !wsum || !wcount || !curve)
        goto done;
    for (unsigned k = 0; k < half; k++) {
        unsigned lo = k > 2 ? k - 2 : 0, hi = k + 3;
        double idx = threshold_index(e, spec, half, lo, hi, k);
        target[k] = (idx - 255) * 140.0 / 255;  /* decibels, 0 at full scale */
    }
    /* A filter of low order cannot follow a range of more than 100 dB, and
     * where it falls far below a peak the residue would overflow. Bins
     * more than 60 dB below the loudest are quantised to zero in any case,
     * and the target ends there. */
    double tmax = -1e9;
    for (unsigned k = 0; k < half; k++)
        tmax = fmax(tmax, target[k]);
    for (unsigned k = 0; k < half; k++)
        target[k] = fmax(target[k], tmax - 60);
    /* The upper envelope on the Bark scale: the largest target in each
     * Bark bin, which maintains the fitted curve on the spectral peaks. */
    for (unsigned k = 0; k < half; k++) {
        double s = target[k] + FLOOR0_OFFSET;
        s = s < 1 ? 1 : s;
        if (s > wsum[map[k]])
            wsum[map[k]] = s;
        wcount[map[k]] += 1;
    }
    /* A short block leaves Bark bins without a bin at low frequencies.
     * They are filled by linear interpolation between their filled
     * neighbours, which samples the whole axis evenly for the fit. */
    int last = -1;
    for (int j = 0; j < FLOOR0_BARK; j++) {
        if (!wcount[j])
            continue;
        for (int g = last + 1; g < j; g++)
            wsum[g] = last < 0 ? wsum[j] : wsum[last] + (wsum[j] - wsum[last]) * (g - last) / (j - last);
        last = j;
    }
    for (int g = last + 1; last >= 0 && g < FLOOR0_BARK; g++)
        wsum[g] = wsum[last];
    double r[VB_MAX_ORDER + 1] = { 0 };
    for (unsigned j = 0; j < FLOOR0_BARK; j++) {
        double s = wsum[j], w = M_PI * j / FLOOR0_BARK;
        for (unsigned m = 0; m <= order; m++)
            r[m] += s * s * cos(m * w);
    }
    r[0] *= 1.0001;
    double a[VB_MAX_ORDER + 2] = { 1 }, err = r[0];
    for (unsigned i = 1; i <= order && err > 0; i++) {
        double acc = r[i];
        for (unsigned j = 1; j < i; j++)
            acc += a[j] * r[i - j];
        double k = -acc / err, prev[VB_MAX_ORDER + 2];
        memcpy(prev, a, sizeof prev);
        for (unsigned j = 1; j < i; j++)
            a[j] = prev[j] + k * prev[i - j];
        a[i] = k;
        err *= 1 - k * k;
    }
    /* P(z) = A(z) + z^-(p+1) A(1/z) and Q(z) = A(z) - z^-(p+1) A(1/z),
     * evaluated on the unit circle as real functions of the angle. */
    double pc[VB_MAX_ORDER + 2], qc[VB_MAX_ORDER + 2];
    for (unsigned i = 0; i <= order + 1; i++) {
        double ai = i <= order ? a[i] : 0, ar = order + 1 - i <= order ? a[order + 1 - i] : 0;
        pc[i] = ai + ar;
        qc[i] = ai - ar;
    }
    double roots[VB_MAX_ORDER];
    unsigned nroots = 0;
    for (int poly = 0; poly < 2; poly++) {
        const double *c = poly ? qc : pc;
        double prevv = 0, prevw = 0;
        unsigned steps = 4096;
        for (unsigned s = 0; s <= steps; s++) {
            double w = M_PI * s / steps;
            double v = poly_value(c, order, w, poly);
            if (s && ((prevv < 0 && v >= 0) || (prevv > 0 && v <= 0)) && w > 1e-9 && w < M_PI - 1e-9 &&
                nroots < order) {
                double lo = prevw, hi = w, flo = prevv;
                for (int it = 0; it < 40; it++) {
                    double mid = (lo + hi) / 2, fm = poly_value(c, order, mid, poly);
                    if ((flo < 0) == (fm < 0)) {
                        lo = mid;
                        flo = fm;
                    } else {
                        hi = mid;
                    }
                }
                roots[nroots++] = (lo + hi) / 2;
            }
            prevv = v;
            prevw = w;
        }
    }
    for (unsigned i = 1; i < nroots; i++)
        for (unsigned j = i; j > 0 && roots[j - 1] > roots[j]; j--) {
            double t = roots[j];
            roots[j] = roots[j - 1];
            roots[j - 1] = t;
        }
    while (nroots < order)
        roots[nroots] = nroots ? roots[nroots - 1] : 0.1, nroots++;
    /* Quantise the cumulative angles to the step of the book. */
    double delta = vb_float32(pack_float(M_PI / LSP_ENTRIES));
    long prevq = 0;
    struct vb_floor_data d;
    for (unsigned i = 0; i < order; i++) {
        long q = rnd(roots[i] / delta);
        if (q < prevq)
            q = prevq;
        if (q - prevq >= LSP_ENTRIES)
            q = prevq + LSP_ENTRIES - 1;
        out[1 + i] = (uint16_t)(q - prevq);
        d.coef[i] = (float)(q * delta);
        prevq = q;
    }
    /* The amplitude: the decoder's curve in decibels is amp * c - offset,
     * with c computed here from the curve at amplitude 1. The amplitude
     * maintains the curve at or below the target at all but 2% of the bins,
     * because a floor above the masking estimate lets audible
     * quantisation noise through. */
    d.amplitude = 1;
    vb_floor_render(floor, &d, blockflag, 2 * half, curve);
    double *limit = wsum;               /* reused: FLOOR0_BARK >= the bins needed */
    unsigned nlimit = 0;
    for (unsigned k = 0; k < half; k++) {
        double c1 = 20 * log10(fmax(curve[k], 1e-30)) + FLOOR0_OFFSET;
        if (c1 > 1e-6 && nlimit < FLOOR0_BARK)
            limit[nlimit++] = (target[k] + FLOOR0_OFFSET) / c1;
        else if (c1 > 1e-6) {
            /* Retain the smallest values seen. */
            unsigned big = 0;
            for (unsigned i = 1; i < nlimit; i++)
                if (limit[i] > limit[big])
                    big = i;
            double v = (target[k] + FLOOR0_OFFSET) / c1;
            if (v < limit[big])
                limit[big] = v;
        }
    }
    for (unsigned i = 1; i < nlimit; i++)
        for (unsigned j = i; j > 0 && limit[j - 1] > limit[j]; j--) {
            double t = limit[j];
            limit[j] = limit[j - 1];
            limit[j - 1] = t;
        }
    long amp = nlimit ? (long)limit[nlimit * 2 / 100] : 1;
    long top = (1 << FLOOR0_AMP_BITS) - 1;
    amp = amp < 1 ? 1 : amp > top ? top : amp;
    /* Raise the amplitude while the residue would exceed the books. */
    for (;;) {
        d.amplitude = (int)amp;
        vb_floor_render(floor, &d, blockflag, 2 * half, curve);
        double worst = 0;
        for (unsigned k = 0; k < half; k++)
            worst = fmax(worst, fabs(spec[k]) / fmax(curve[k], 1e-30));
        if (worst <= MAX_RESIDUE || amp == top)
            break;
        amp++;
    }
    out[0] = (uint16_t)amp;
done:
    free(target);
    free(wsum);
    free(wcount);
    free(curve);
}

/* ---- blocks ---- */

/* Short blocks where the energy of 64 samples of the high passed mix rises
 * more than eightfold over the mean of the 512 samples before it. */
static uint8_t *transients(const float *const *x, unsigned channels, long frames, long *nseg)
{
    long n = (frames + 63) / 64;
    double *energy = calloc((size_t)n + 1, sizeof *energy);
    uint8_t *mark = calloc((size_t)n + 1, 1);
    if (!energy || !mark) {
        free(energy);
        free(mark);
        return NULL;
    }
    for (long t = 1; t < frames; t++)
        for (unsigned c = 0; c < channels; c++) {
            double d = x[c][t] - x[c][t - 1];
            energy[t / 64] += d * d;
        }
    for (long s = 8; s < n; s++) {
        double mean = 0;
        for (long j = s - 8; j < s; j++)
            mean += energy[j];
        mean /= 8;
        mark[s] = energy[s] > 8 * mean + 1e-6;
    }
    free(energy);
    *nseg = n;
    return mark;
}

static int is_transient(const uint8_t *mark, long nseg, long from, long to)
{
    for (long s = from < 0 ? 0 : from / 64; s <= to / 64 && s < nseg; s++)
        if (mark[s])
            return 1;
    return 0;
}

static int plan_blocks(struct encoder *e, const float *const *x, long frames)
{
    long nseg;
    uint8_t *mark = transients(x, e->channels, frames, &nseg);
    if (!mark)
        return -ENOMEM;
    long cap = frames / 128 + 16;
    e->blocks = calloc((size_t)cap, sizeof *e->blocks);
    if (!e->blocks) {
        free(mark);
        return -ENOMEM;
    }
    long center = 0;
    unsigned flag = 1;
    for (long i = 0;; i++) {
        if (i == cap) {
            free(mark);
            return -EINVAL;
        }
        e->blocks[i].flag = flag;
        e->blocks[i].center = center;
        e->nblocks = i + 1;
        if (center >= frames && i >= 1)
            break;
        /* The next block is short when a transient lies within the span of
         * a long block around its centre. */
        long next_long = center + e->setup.blocksize[flag] / 4 + LONG / 4;
        unsigned next = is_transient(mark, nseg, next_long - LONG / 2, next_long + LONG / 2) ? 0 : 1;
        center += e->setup.blocksize[flag] / 4 + e->setup.blocksize[next] / 4;
        flag = next;
    }
    for (long i = 0; i < e->nblocks; i++) {
        e->blocks[i].prevflag = i ? e->blocks[i - 1].flag : 1;
        e->blocks[i].nextflag = i + 1 < e->nblocks ? e->blocks[i + 1].flag : 1;
    }
    free(mark);
    return 0;
}

/* The forward square polar coupling, the inverse of the decoder's step. */
static void couple(int l, int r, int *m, int *a)
{
    if (l > 0 && l > r) {
        *m = l;
        *a = l - r;
    } else if (r > 0 && r >= l) {
        *m = r;
        *a = l - r;
    } else if (r > l) {
        *m = l;
        *a = r - l;
    } else {
        *m = r;
        *a = r - l;
    }
}

/* The class of a partition by the largest magnitude in it. Class 4 codes
 * values up to 496 as a multiple of 16 and a rest, class 5 values up to
 * 8190 as a multiple of 256, a multiple of 16 and a rest. */
static unsigned classify(const int16_t *v, unsigned n)
{
    int top = 0;
    for (unsigned i = 0; i < n; i++)
        top = abs(v[i]) > top ? abs(v[i]) : top;
    return top == 0 ? 0 : top <= 1 ? 1 : top <= 4 ? 2 : top <= 15 ? 3 : top <= 496 ? 4 : 5;
}

/* The part of value x that a pass of a cascade class codes. */
static int cascade_part(int x, unsigned cls, unsigned pass)
{
    if (cls == 4) {
        int coarse = 16 * (int)rnd(x / 16.0);
        coarse = coarse > 496 ? 496 : coarse < -496 ? -496 : coarse;
        return pass == 0 ? coarse : x - coarse;
    }
    int huge = 256 * (int)rnd(x / 256.0);
    huge = huge > 8192 ? 8192 : huge < -8192 ? -8192 : huge;
    int rest = x - huge, mid = 16 * (int)rnd(rest / 16.0);
    mid = mid > 128 ? 128 : mid < -128 ? -128 : mid;
    return pass == 0 ? huge : pass == 1 ? mid : rest - mid;
}

static unsigned grid_entry(unsigned book, const int *v)
{
    unsigned e = 0, mul = 1;
    for (unsigned i = 0; i < grids[book].dims; i++) {
        e += (unsigned)((v[i] - grids[book].min) / grids[book].delta) * mul;
        mul *= grids[book].values;
    }
    return e;
}

/* Visit the vectors of a partition of class cls in the order the decoder
 * reads them: per pass, the book and the entry of each vector. */
static void partition_entries(const int16_t *v, unsigned cls, unsigned pass,
                              void (*emit)(void *, unsigned book, unsigned entry), void *arg)
{
    static const int books[CLASSES][3] = { { -1, -1, -1 }, { BOOK_C1, -1, -1 }, { BOOK_C2, -1, -1 },
                                           { BOOK_C3, -1, -1 }, { BOOK_COARSE, BOOK_FINE, -1 },
                                           { BOOK_HUGE, BOOK_MID, BOOK_FINE } };
    int book = pass < 3 ? books[cls][pass] : -1;
    if (book < 0)
        return;
    unsigned dims = grids[book].dims;
    for (unsigned i = 0; i < PARTITION; i += dims) {
        int vec[4];
        for (unsigned k = 0; k < dims; k++)
            vec[k] = cls >= 4 ? cascade_part(v[i + k], cls, pass) : v[i + k];
        emit(arg, (unsigned)book, grid_entry((unsigned)book, vec));
    }
}

static void count_entry(void *arg, unsigned book, unsigned entry)
{
    struct encoder *e = arg;
    e->books[book].count[entry]++;
}

/* Analyse one block: floors, residue, coupling, classes, counts. */
static int analyse(struct encoder *e, struct block *b, const float *const *x, long frames, float *buf, float *spec)
{
    unsigned n = e->setup.blocksize[b->flag], half = n / 2, ch = e->channels;
    const struct vb_floor *floor = &e->setup.floors[b->flag];
    unsigned stereo = ch == 2;
    unsigned ylen = e->posts[b->flag], llen = e->order[b->flag] + 1;
    b->unused = calloc(ch, 1);
    b->res = calloc((size_t)half * ch, sizeof *b->res);
    unsigned vectors = stereo ? 1 : ch, len = stereo ? half * 2 : half, parts = len / PARTITION;
    b->cls = calloc((size_t)vectors * (parts + 2), 1);
    if (e->floor_type == 1)
        b->y = calloc((size_t)ylen * ch, 1);
    else
        b->lsp = calloc((size_t)llen * ch, sizeof *b->lsp);
    float *curve = malloc(sizeof *curve * half);
    if (!b->unused || !b->res || !b->cls || (!b->y && !b->lsp) || !curve) {
        free(curve);
        return -ENOMEM;
    }
    int *q = malloc(sizeof *q * half * ch);
    if (!q) {
        free(curve);
        return -ENOMEM;
    }
    long start = b->center - (long)half;
    for (unsigned c = 0; c < ch; c++) {
        for (unsigned i = 0; i < n; i++) {
            long t = start + (long)i;
            buf[i] = t >= 0 && t < frames ? x[c][t] : 0;
        }
        vb_window(e->ramp, e->setup.blocksize, buf, n, b->flag, b->prevflag, b->nextflag);
        codec_mdct(&e->mdct[b->flag], buf, spec);
        for (unsigned k = 0; k < half; k++)
            spec[k] *= 4.0f / n;
        struct vb_floor_data d;
        if (e->floor_type == 1) {
            /* Lift the floor until no residue exceeds the books, which can
             * happen between the posts or at the highest qualities. */
            uint8_t *y = b->y + (size_t)c * ylen;
            for (int lift = 0; lift <= 64; lift += 2) {
                floor1_encode(e, &floor->u.f1, spec, half, lift, y);
                for (unsigned i = 0; i < floor->u.f1.values; i++)
                    d.y[i] = y[i];
                vb_floor_render(floor, &d, b->flag, n, curve);
                double worst = 0;
                for (unsigned k = 0; k < half; k++)
                    worst = fmax(worst, fabs(spec[k]) / fmax(curve[k], 1e-30));
                if (worst < MAX_RESIDUE + 0.5)
                    break;
            }
        } else {
            uint16_t *l = b->lsp + (size_t)c * llen;
            floor0_encode(e, &floor->u.f0, b->flag, spec, half, l, floor);
            d.amplitude = l[0];
            double delta = vb_float32(pack_float(M_PI / LSP_ENTRIES));
            long acc = 0;
            for (unsigned i = 0; i < floor->u.f0.order; i++) {
                acc += l[1 + i];
                d.coef[i] = (float)(acc * delta);
            }
        }
        vb_floor_render(floor, &d, b->flag, n, curve);
        int any = 0;
        for (unsigned k = 0; k < half; k++) {
            double r = spec[k] / fmax(curve[k], 1e-30);
            long v = rnd(r);
            v = v > MAX_RESIDUE ? MAX_RESIDUE : v < -MAX_RESIDUE ? -MAX_RESIDUE : v;
            q[c * half + k] = (int)v;
            any |= v != 0;
        }
        b->unused[c] = !any;
        if (!any)
            memset(q + c * half, 0, sizeof *q * half);
    }
    /* Coupling and the order of the residue vectors. */
    if (stereo) {
        for (unsigned k = 0; k < half; k++) {
            int m, a;
            couple(q[k], q[half + k], &m, &a);
            b->res[2 * k] = (int16_t)m;
            b->res[2 * k + 1] = (int16_t)a;
        }
    } else {
        for (unsigned i = 0; i < half * ch; i++)
            b->res[i] = (int16_t)q[i];
    }
    free(q);
    free(curve);
    /* Counts: the floors, the classes and the residue vectors. */
    for (unsigned c = 0; c < ch; c++) {
        if (b->unused[c])
            continue;
        if (e->floor_type == 1) {
            const uint8_t *y = b->y + (size_t)c * ylen;
            for (unsigned i = 2; i < floor->u.f1.values; i++)
                e->books[BOOK_Y].count[y[i]]++;
        } else {
            const uint16_t *l = b->lsp + (size_t)c * llen;
            for (unsigned i = 0; i < floor->u.f0.order; i++)
                e->books[BOOK_LSP].count[l[1 + i]]++;
        }
    }
    int all_unused = 1;
    for (unsigned c = 0; c < ch; c++)
        all_unused &= b->unused[c];
    for (unsigned v = 0; v < vectors; v++) {
        int skip = stereo ? all_unused : b->unused[v];
        if (skip)
            continue;
        const int16_t *vec = b->res + (size_t)v * len;
        uint8_t *cls = b->cls + (size_t)v * (parts + 2);
        for (unsigned p = 0; p < parts; p++)
            cls[p] = (uint8_t)classify(vec + p * PARTITION, PARTITION);
        for (unsigned p = 0; p < parts; p += 2)
            e->books[BOOK_CLASS].count[cls[p] * CLASSES + cls[p + 1]]++;
        for (unsigned pass = 0; pass < 3; pass++)
            for (unsigned p = 0; p < parts; p++)
                partition_entries(vec + p * PARTITION, cls[p], pass, count_entry, e);
    }
    return 0;
}

/* ---- the headers ---- */

static void write_book(struct vb_writer *w, const struct code *c, unsigned book)
{
    put(w, 0x564342, 24);
    /* The classbook codes the classes of two partitions per codeword. */
    unsigned dims = book == BOOK_CLASS ? 2 : book == BOOK_Y || book == BOOK_LSP ? 1 : grids[book].dims;
    put(w, dims, 16);
    put(w, c->entries, 24);
    put(w, 0, 1);                       /* not ordered */
    int sparse = 0;
    for (unsigned i = 0; i < c->entries; i++)
        sparse |= !c->len[i];
    put(w, (uint32_t)sparse, 1);
    for (unsigned i = 0; i < c->entries; i++) {
        if (sparse)
            put(w, c->len[i] != 0, 1);
        if (c->len[i])
            put(w, c->len[i] - 1u, 5);
    }
    if (book == BOOK_Y || book == BOOK_CLASS) {
        put(w, 0, 4);
        return;
    }
    put(w, 1, 4);                       /* lookup type 1 */
    if (book == BOOK_LSP) {
        put(w, pack_float(0), 32);
        put(w, pack_float(M_PI / LSP_ENTRIES), 32);
        put(w, 7, 4);                   /* 8 bit multiplicands */
        put(w, 0, 1);
        for (unsigned i = 0; i < LSP_ENTRIES; i++)
            put(w, i, 8);
        return;
    }
    put(w, pack_float(grids[book].min), 32);
    put(w, pack_float(grids[book].delta), 32);
    unsigned bits = vb_ilog(grids[book].values - 1);
    put(w, bits - 1, 4);
    put(w, 0, 1);
    for (unsigned i = 0; i < grids[book].values; i++)
        put(w, i, bits);
}

static void write_setup(struct vb_writer *w, const struct encoder *e)
{
    put(w, 5, 8);
    put_bytes(w, "vorbis", 6);
    put(w, NBOOKS - 1, 8);
    for (unsigned b = 0; b < NBOOKS; b++)
        write_book(w, &e->books[b], b);
    put(w, 0, 6);                       /* one time domain transform */
    put(w, 0, 16);
    put(w, 1, 6);                       /* two floors */
    for (unsigned f = 0; f < 2; f++) {
        const struct vb_floor *fl = &e->setup.floors[f];
        put(w, (uint32_t)fl->type, 16);
        if (fl->type == 0) {
            const struct vb_floor0 *g = &fl->u.f0;
            put(w, g->order, 8);
            put(w, g->rate, 16);
            put(w, g->bark_map_size, 16);
            put(w, g->amplitude_bits, 6);
            put(w, g->amplitude_offset, 8);
            put(w, 0, 4);
            put(w, BOOK_LSP, 8);
            continue;
        }
        const struct vb_floor1 *g = &fl->u.f1;
        put(w, g->partitions, 5);
        for (unsigned i = 0; i < g->partitions; i++)
            put(w, 0, 4);
        put(w, g->class_dims[0] - 1u, 3);
        put(w, 0, 2);
        put(w, BOOK_Y + 1, 8);
        put(w, g->multiplier - 1, 2);
        unsigned rangebits = vb_ilog(g->x[1]) - 1;
        put(w, rangebits, 4);
        for (unsigned i = 2; i < g->values; i++)
            put(w, g->x[i], rangebits);
    }
    put(w, 1, 6);                       /* two residues */
    for (unsigned r = 0; r < 2; r++) {
        unsigned half = e->setup.blocksize[r] / 2, type = e->channels == 2 ? 2 : 1;
        put(w, type, 16);
        put(w, 0, 24);
        put(w, type == 2 ? half * 2 : half, 24);
        put(w, PARTITION - 1, 24);
        put(w, CLASSES - 1, 6);
        put(w, BOOK_CLASS, 8);
        static const unsigned cascade[CLASSES] = { 0, 1, 1, 1, 3, 7 };
        for (unsigned c = 0; c < CLASSES; c++) {
            put(w, cascade[c] & 7, 3);
            put(w, 0, 1);
        }
        static const unsigned books[CLASSES][3] = { { 0 }, { BOOK_C1 }, { BOOK_C2 }, { BOOK_C3 },
                                                    { BOOK_COARSE, BOOK_FINE }, { BOOK_HUGE, BOOK_MID, BOOK_FINE } };
        for (unsigned c = 0; c < CLASSES; c++)
            for (unsigned pass = 0; pass < 3; pass++)
                if (cascade[c] & (1u << pass))
                    put(w, books[c][pass], 8);
    }
    put(w, 1, 6);                       /* two mappings */
    for (unsigned m = 0; m < 2; m++) {
        put(w, 0, 16);
        put(w, 0, 1);                   /* one submap */
        if (e->channels == 2) {
            put(w, 1, 1);
            put(w, 0, 8);               /* one coupling step: 0 with 1 */
            put(w, 0, 1);
            put(w, 1, 1);
        } else {
            put(w, 0, 1);
        }
        put(w, 0, 2);
        put(w, 0, 8);
        put(w, m, 8);                   /* floor */
        put(w, m, 8);                   /* residue */
    }
    put(w, 1, 6);                       /* two modes */
    for (unsigned m = 0; m < 2; m++) {
        put(w, m, 1);
        put(w, 0, 16);
        put(w, 0, 16);
        put(w, m, 8);
    }
    put(w, 1, 1);
}

/* ---- packets ---- */

struct emit_state {
    struct vb_writer *w;
    const struct encoder *e;
};

static void write_entry(void *arg, unsigned book, unsigned entry)
{
    struct emit_state *st = arg;
    put_code(st->w, &st->e->books[book], entry);
}

static void write_packet(struct vb_writer *w, const struct encoder *e, const struct block *b)
{
    unsigned ch = e->channels, half = e->setup.blocksize[b->flag] / 2, stereo = ch == 2;
    const struct vb_floor *floor = &e->setup.floors[b->flag];
    put(w, 0, 1);
    put(w, b->flag, 1);                 /* the mode number, one bit for two modes */
    if (b->flag) {
        put(w, b->prevflag, 1);
        put(w, b->nextflag, 1);
    }
    for (unsigned c = 0; c < ch; c++) {
        if (e->floor_type == 1) {
            if (b->unused[c]) {
                put(w, 0, 1);
                continue;
            }
            const uint8_t *y = b->y + (size_t)c * e->posts[b->flag];
            put(w, 1, 1);
            put(w, y[0], 7);
            put(w, y[1], 7);
            for (unsigned i = 2; i < floor->u.f1.values; i++)
                put_code(w, &e->books[BOOK_Y], y[i]);
        } else {
            if (b->unused[c]) {
                put(w, 0, FLOOR0_AMP_BITS);
                continue;
            }
            const uint16_t *l = b->lsp + (size_t)c * (e->order[b->flag] + 1);
            put(w, l[0], FLOOR0_AMP_BITS);
            put(w, 0, 1);               /* book number, ilog(1) bits */
            for (unsigned i = 0; i < floor->u.f0.order; i++)
                put_code(w, &e->books[BOOK_LSP], l[1 + i]);
        }
    }
    unsigned vectors = stereo ? 1 : ch, len = stereo ? half * 2 : half, parts = len / PARTITION;
    int all_unused = 1;
    for (unsigned c = 0; c < ch; c++)
        all_unused &= b->unused[c];
    struct emit_state st = { w, e };
    for (unsigned pass = 0; pass < 8; pass++)
        for (unsigned p = 0; p < parts; p += 2)
            for (unsigned i = 0; i < 2; i++) {
                for (unsigned v = 0; v < vectors && pass == 0 && i == 0; v++) {
                    int skip = stereo ? all_unused : b->unused[v];
                    if (!skip) {
                        const uint8_t *cls = b->cls + (size_t)v * (parts + 2);
                        put_code(w, &e->books[BOOK_CLASS], cls[p] * CLASSES + cls[p + 1]);
                    }
                }
                if (p + i >= parts)
                    continue;
                for (unsigned v = 0; v < vectors; v++) {
                    int skip = stereo ? all_unused : b->unused[v];
                    if (skip)
                        continue;
                    const uint8_t *cls = b->cls + (size_t)v * (parts + 2);
                    partition_entries(b->res + (size_t)v * len + (p + i) * PARTITION, cls[p + i], pass, write_entry,
                                      &st);
                }
            }
}

/* ---- the encoder entry ---- */

static void free_encoder(struct encoder *e)
{
    for (long i = 0; e->blocks && i < e->nblocks; i++) {
        free(e->blocks[i].unused);
        free(e->blocks[i].y);
        free(e->blocks[i].lsp);
        free(e->blocks[i].res);
        free(e->blocks[i].cls);
    }
    free(e->blocks);
    for (unsigned b = 0; b < NBOOKS; b++) {
        free(e->books[b].count);
        free(e->books[b].len);
        free(e->books[b].word);
    }
    for (int i = 0; i < 2; i++) {
        codec_mdct_free(&e->mdct[i]);
        free(e->ramp[i]);
        if (e->setup.floors && e->setup.floors[i].type == 0) {
            free(e->setup.floors[i].u.f0.map[0]);
            free(e->setup.floors[i].u.f0.map[1]);
        }
    }
    free(e->setup.floors);
}

long vorbis_encode_options(const struct codec_audio_format *fmt, const int32_t *samples, long frames,
                           const char *options, uint8_t **result)
{
    char value[32];
    if (codec_options_check(options, "quality floor") < 0)
        return -EINVAL;
    struct encoder e = { 0 };
    e.channels = (unsigned)fmt->channels;
    e.rate = (unsigned)fmt->rate;
    e.quality = 0.4;
    e.floor_type = 1;
    if (codec_option(options, "quality", value, sizeof value))
        e.quality = atof(value);
    if (codec_option(options, "floor", value, sizeof value))
        e.floor_type = (unsigned)atoi(value);
    /* Floor 0 stores the sample rate in 16 bits. */
    if (e.quality < -0.1 || e.quality > 1.0 || e.floor_type > 1 || e.channels < 1 || e.channels > 255 ||
        fmt->rate <= 0 || (e.floor_type == 0 && fmt->rate > 65535))
        return -EINVAL;
    e.setup.channels = e.channels;
    e.setup.rate = e.rate;
    e.setup.blocksize[0] = SHORT;
    e.setup.blocksize[1] = LONG;
    e.setup.nfloors = 2;
    e.setup.floors = calloc(2, sizeof *e.setup.floors);
    long err = e.setup.floors ? 0 : -ENOMEM;
    for (int b = 0; b < 2 && !err; b++) {
        unsigned half = e.setup.blocksize[b] / 2;
        struct vb_floor *f = &e.setup.floors[b];
        f->type = (int)e.floor_type;
        if (e.floor_type == 1) {
            floor1_layout(&f->u.f1, half, b ? 60 : 16);
            floor1_neighbours(&f->u.f1);
            e.posts[b] = f->u.f1.values;
        } else {
            struct vb_floor0 *g = &f->u.f0;
            g->order = b ? 24 : 12;
            g->rate = e.rate;
            g->bark_map_size = FLOOR0_BARK;
            g->amplitude_bits = FLOOR0_AMP_BITS;
            g->amplitude_offset = FLOOR0_OFFSET;
            g->nbooks = 1;
            g->books[0] = BOOK_LSP;
            e.order[b] = g->order;
            if (vb_floor0_maps(g, e.setup.blocksize) < 0)
                err = -ENOMEM;
        }
        e.ramp[b] = malloc(sizeof *e.ramp[b] * half);
        if (!e.ramp[b] || codec_mdct_init(&e.mdct[b], e.setup.blocksize[b]) < 0)
            err = -ENOMEM;
        else
            vb_window_ramp(e.ramp[b], half);
    }
    for (unsigned b = 0; b < NBOOKS && !err; b++) {
        struct code *c = &e.books[b];
        c->entries = book_entries(b);
        c->count = calloc(c->entries, sizeof *c->count);
        c->len = calloc(c->entries, 1);
        c->word = calloc(c->entries, sizeof *c->word);
        if (!c->count || !c->len || !c->word)
            err = -ENOMEM;
    }
    float **x = calloc(e.channels, sizeof *x);
    float *buf = malloc(sizeof *buf * LONG), *spec = malloc(sizeof *spec * LONG / 2);
    if (!x || !buf || !spec)
        err = -ENOMEM;
    /* Vorbis channel c is the WAV channel that the decoder returns in its
     * place (section 4.3.9 of the specification and decode.c). */
    static const unsigned maps[9][8] = {
        { 0 }, { 0 }, { 0, 1 }, { 0, 2, 1 }, { 0, 1, 2, 3 }, { 0, 2, 1, 3, 4 }, { 0, 2, 1, 5, 3, 4 },
        { 0, 2, 1, 6, 5, 3, 4 }, { 0, 2, 1, 7, 5, 6, 3, 4 },
    };
    for (unsigned c = 0; c < e.channels && !err; c++) {
        unsigned from = c;
        for (unsigned k = 0; e.channels <= 8 && k < e.channels; k++)
            if (maps[e.channels][k] == c)
                from = k;
        x[c] = malloc(sizeof **x * (size_t)(frames ? frames : 1));
        if (!x[c])
            err = -ENOMEM;
        else
            for (long t = 0; t < frames; t++)
                x[c][t] = (float)(samples[t * e.channels + from] / 2147483648.0);
    }
    if (!err)
        err = plan_blocks(&e, (const float *const *)x, frames);
    for (long i = 0; i < e.nblocks && !err; i++)
        err = analyse(&e, &e.blocks[i], (const float *const *)x, frames, buf, spec);
    for (unsigned c = 0; x && c < e.channels; c++)
        free(x[c]);
    free(x);
    free(buf);
    free(spec);
    for (unsigned b = 0; b < NBOOKS && !err; b++)
        err = huffman(&e.books[b]);
    if (err) {
        free_encoder(&e);
        return err;
    }

    /* The stream. */
    struct codec_ogg_writer og;
    codec_ogg_writer_init(&og, 0x6d696e69u ^ (uint32_t)frames);
    struct vb_writer w = { 0 };
    put(&w, 1, 8);
    put_bytes(&w, "vorbis", 6);
    put(&w, 0, 32);
    put(&w, e.channels, 8);
    put(&w, e.rate, 32);
    put(&w, 0, 32);
    put(&w, 0, 32);
    put(&w, 0, 32);
    put(&w, 0xb8, 8);                   /* block sizes 256 and 2048 */
    put(&w, 1, 1);
    codec_ogg_write_packet(&og, w.data, w.len, 0);
    codec_ogg_flush(&og, 0);
    w.len = w.bit = 0;
    static const char vendor[] = "minios libcodec";
    put(&w, 3, 8);
    put_bytes(&w, "vorbis", 6);
    put(&w, sizeof vendor - 1, 32);
    put_bytes(&w, vendor, sizeof vendor - 1);
    put(&w, 0, 32);
    put(&w, 1, 1);
    codec_ogg_write_packet(&og, w.data, w.len, 0);
    w.len = w.bit = 0;
    write_setup(&w, &e);
    codec_ogg_write_packet(&og, w.data, w.len, 0);
    codec_ogg_flush(&og, 0);
    for (long i = 0; i < e.nblocks; i++) {
        w.len = w.bit = 0;
        write_packet(&w, &e, &e.blocks[i]);
        long granule = e.blocks[i].center < frames ? e.blocks[i].center : frames;
        codec_ogg_write_packet(&og, w.data, w.len, granule);
    }
    codec_ogg_flush(&og, 1);
    err = w.failed || og.failed ? -ENOMEM : 0;
    free(w.data);
    free_encoder(&e);
    if (err) {
        codec_ogg_writer_free(&og);
        return err;
    }
    free(og.body);
    *result = og.out;
    return (long)og.len;
}

long vorbis_encode(const struct codec_audio_format *fmt, const int32_t *samples, long frames, uint8_t **result)
{
    return vorbis_encode_options(fmt, samples, frames, NULL, result);
}
