/* FLAC decoding. flac_open reads the metadata blocks, and flac_read
 * decodes one frame at a time into a buffer of interleaved samples from
 * which it copies the requested frames. Every frame header is checked
 * against its CRC-8 and every frame against its CRC-16, and at the end
 * of the stream the MD5 sum of the decoded samples is compared with the
 * sum in STREAMINFO, as `flac -t` does. A damaged stream makes the read
 * that reaches the damage return -EBADMSG after the frames decoded before
 * it. */
#include "flac.h"

struct flac_state {
    const uint8_t *data;
    size_t len, pos;                    /* pos: the byte offset of the next frame */
    struct flac_streaminfo si;
    int64_t *ch[FLAC_MAX_CHANNELS];     /* the samples of the current frame */
    unsigned cap;                       /* frames that the buffers can hold */
    int32_t *out;                       /* the current frame, interleaved, full scale */
    unsigned out_n, out_pos;
    struct codec_md5 md5;
    uint64_t decoded;
    int ended, error;
};

static int grow(struct flac_state *s, unsigned n)
{
    if (n <= s->cap)
        return 0;
    for (unsigned c = 0; c < s->si.channels; c++) {
        int64_t *p = realloc(s->ch[c], (size_t)n * sizeof *p);
        if (!p)
            return -ENOMEM;
        s->ch[c] = p;
    }
    int32_t *o = realloc(s->out, (size_t)n * s->si.channels * sizeof *o);
    if (!o)
        return -ENOMEM;
    s->out = o;
    s->cap = n;
    return 0;
}

/* ---- the frame header ---- */

struct header {
    unsigned block, rate, channels, assignment, bits;
    size_t length;                      /* bytes including the CRC-8 */
};

/* The number coded like UTF-8 in up to seven bytes. */
static int coded_number(struct flac_reader *r, uint64_t *v)
{
    unsigned b = (unsigned)flac_bits(r, 8), extra;
    if (b < 0x80) {
        *v = b;
        return 0;
    }
    if (b < 0xc0 || b == 0xff)
        return -EINVAL;
    for (extra = 1; b & (0x80 >> (extra + 1)); extra++)
        ;
    uint64_t value = b & (0x3fu >> extra);
    for (unsigned i = 0; i < extra; i++) {
        unsigned c = (unsigned)flac_bits(r, 8);
        if ((c & 0xc0) != 0x80)
            return -EINVAL;
        value = value << 6 | (c & 0x3f);
    }
    *v = value;
    return r->failed ? -EINVAL : 0;
}

/* Parse the frame header at p. Returns 0, or -EINVAL when p holds no
 * valid header. */
static int parse_header(const uint8_t *p, size_t len, const struct flac_streaminfo *si, struct header *h)
{
    struct flac_reader r = { p, len, 0, 0 };
    if (flac_bits(&r, 15) != 0x7ffc)
        return -EINVAL;
    flac_bits(&r, 1);                   /* the blocking strategy */
    unsigned bs = (unsigned)flac_bits(&r, 4), rate = (unsigned)flac_bits(&r, 4);
    unsigned assignment = (unsigned)flac_bits(&r, 4), size = (unsigned)flac_bits(&r, 3);
    if (flac_bits(&r, 1) || bs == 0 || rate == 15 || assignment > FLAC_MID_SIDE || size == 3)
        return -EINVAL;
    uint64_t number;
    if (coded_number(&r, &number) < 0)
        return -EINVAL;
    if (bs == 6)
        h->block = (unsigned)flac_bits(&r, 8) + 1;
    else if (bs == 7)
        h->block = (unsigned)flac_bits(&r, 16) + 1;
    else
        h->block = flac_block_sizes[bs];
    if (rate == 12)
        h->rate = (unsigned)flac_bits(&r, 8) * 1000;
    else if (rate == 13)
        h->rate = (unsigned)flac_bits(&r, 16);
    else if (rate == 14)
        h->rate = (unsigned)flac_bits(&r, 16) * 10;
    else
        h->rate = rate ? flac_rates[rate] : si->rate;
    size_t crc_at = r.pos / 8;
    unsigned crc = (unsigned)flac_bits(&r, 8);
    if (r.failed || crc != flac_crc8(p, crc_at))
        return -EINVAL;
    h->assignment = assignment;
    h->channels = assignment < 8 ? assignment + 1 : 2;
    h->bits = size ? flac_sample_sizes[size] : si->bits;
    h->length = crc_at + 1;
    return 0;
}

/* ---- subframes ---- */

static int residual(struct flac_reader *r, int64_t *s, unsigned block, unsigned order)
{
    unsigned method = (unsigned)flac_bits(r, 2);
    if (method > 1)
        return -EINVAL;
    unsigned pbits = method ? 5 : 4, escape = method ? 31 : 15;
    unsigned porder = (unsigned)flac_bits(r, 4);
    unsigned psize = block >> porder;
    if ((psize << porder) != block || psize < order)
        return -EINVAL;
    unsigned i = order;
    for (unsigned p = 0; p < 1u << porder; p++) {
        unsigned n = p ? psize : psize - order;
        unsigned k = (unsigned)flac_bits(r, pbits);
        if (k == escape) {
            unsigned w = (unsigned)flac_bits(r, 5);
            for (unsigned j = 0; j < n; j++)
                s[i++] = w ? flac_sbits(r, w) : 0;
        } else {
            for (unsigned j = 0; j < n; j++) {
                uint32_t q = flac_unary(r);
                if (q > UINT32_MAX >> k || r->failed)
                    return -EINVAL;
                uint64_t v = (uint64_t)q << k | flac_bits(r, k);
                s[i++] = (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
            }
        }
        if (r->failed)
            return -EINVAL;
    }
    return 0;
}

static int subframe(struct flac_reader *r, int64_t *s, unsigned block, unsigned bits)
{
    if (flac_bits(r, 1))
        return -EINVAL;
    unsigned type = (unsigned)flac_bits(r, 6), wasted = 0;
    if (flac_bits(r, 1)) {
        wasted = flac_unary(r) + 1;
        if (wasted >= bits)
            return -EINVAL;
    }
    unsigned eb = bits - wasted;
    if (type == 0) {
        int64_t v = flac_sbits(r, eb);
        for (unsigned i = 0; i < block; i++)
            s[i] = v;
    } else if (type == 1) {
        for (unsigned i = 0; i < block; i++)
            s[i] = flac_sbits(r, eb);
    } else if (type >= 8 && type <= 12) {
        unsigned order = type - 8;
        if (order > block)
            return -EINVAL;
        for (unsigned i = 0; i < order; i++)
            s[i] = flac_sbits(r, eb);
        if (residual(r, s, block, order) < 0)
            return -EINVAL;
        for (unsigned i = order; i < block; i++) {
            switch (order) {
            case 1: s[i] += s[i - 1]; break;
            case 2: s[i] += 2 * s[i - 1] - s[i - 2]; break;
            case 3: s[i] += 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3]; break;
            case 4: s[i] += 4 * s[i - 1] - 6 * s[i - 2] + 4 * s[i - 3] - s[i - 4]; break;
            default: break;
            }
        }
    } else if (type >= 32) {
        unsigned order = type - 31;
        if (order > block)
            return -EINVAL;
        for (unsigned i = 0; i < order; i++)
            s[i] = flac_sbits(r, eb);
        unsigned precision = (unsigned)flac_bits(r, 4);
        int shift = (int)flac_sbits(r, 5);
        if (precision == 15 || shift < 0)
            return -EINVAL;
        int64_t coef[FLAC_MAX_ORDER];
        for (unsigned j = 0; j < order; j++)
            coef[j] = flac_sbits(r, precision + 1);
        if (residual(r, s, block, order) < 0)
            return -EINVAL;
        for (unsigned i = order; i < block; i++) {
            int64_t sum = 0;
            for (unsigned j = 0; j < order; j++)
                sum += coef[j] * s[i - 1 - j];
            s[i] += sum >> shift;
        }
    } else {
        return -EINVAL;
    }
    if (r->failed)
        return -EINVAL;
    if (wasted)
        for (unsigned i = 0; i < block; i++)
            s[i] = (int64_t)((uint64_t)s[i] << wasted);
    return 0;
}

/* ---- frames ---- */

/* The offset of the next valid frame header at or after from, or len. */
static size_t find_header(const struct flac_state *s, size_t from)
{
    struct header h;
    for (size_t i = from; i + 2 <= s->len; i++)
        if (s->data[i] == 0xff && (s->data[i + 1] & 0xfe) == 0xf8 &&
            parse_header(s->data + i, s->len - i, &s->si, &h) == 0)
            return i;
    return s->len;
}

/* Decode the frame at s->pos into s->out. Returns its block size, 0 at the
 * end of the stream, or -EBADMSG for a damaged stream. */
static int decode_frame(struct flac_state *s)
{
    if (s->pos >= s->len)
        return 0;
    struct header h;
    if (parse_header(s->data + s->pos, s->len - s->pos, &s->si, &h) < 0) {
        /* Bytes after the last frame, such as an ID3v1 tag, end the
         * stream. A valid header further on means a damaged frame. */
        return find_header(s, s->pos + 1) < s->len ? -EBADMSG : 0;
    }
    if (h.channels != s->si.channels || h.bits != s->si.bits || h.block > FLAC_MAX_BLOCK)
        return -EBADMSG;
    if (grow(s, h.block) < 0)
        return -ENOMEM;
    struct flac_reader r = { s->data + s->pos, s->len - s->pos, h.length * 8, 0 };
    for (unsigned c = 0; c < h.channels; c++) {
        unsigned extra = (h.assignment == FLAC_LEFT_SIDE && c == 1) || (h.assignment == FLAC_SIDE_RIGHT && c == 0) ||
                         (h.assignment == FLAC_MID_SIDE && c == 1);
        if (subframe(&r, s->ch[c], h.block, h.bits + extra) < 0)
            return -EBADMSG;
    }
    flac_align(&r);
    size_t crc_at = r.pos / 8;
    unsigned crc = (unsigned)flac_bits(&r, 16);
    if (r.failed || crc != flac_crc16(s->data + s->pos, crc_at))
        return -EBADMSG;
    s->pos += crc_at + 2;

    int64_t *a = s->ch[0], *b = s->ch[1];
    for (unsigned i = 0; i < h.block && h.assignment >= 8; i++) {
        int64_t x = a[i], y = b[i];
        if (h.assignment == FLAC_LEFT_SIDE) {
            b[i] = x - y;
        } else if (h.assignment == FLAC_SIDE_RIGHT) {
            a[i] = x + y;
        } else {
            int64_t mid = (int64_t)((uint64_t)x << 1) | (y & 1);
            a[i] = (mid + y) >> 1;
            b[i] = (mid - y) >> 1;
        }
    }
    flac_md5_samples(&s->md5, (const int64_t *const *)s->ch, h.channels, h.block, h.bits);
    unsigned shift = 32 - h.bits;
    for (unsigned i = 0; i < h.block; i++)
        for (unsigned c = 0; c < h.channels; c++)
            s->out[i * h.channels + c] = (int32_t)((uint64_t)s->ch[c][i] << shift);
    s->out_n = h.block;
    s->out_pos = 0;
    s->decoded += h.block;
    return (int)h.block;
}

/* At the end of the stream: the number of samples and the MD5 sum must
 * match STREAMINFO when it records them. */
static int finish(struct flac_state *s)
{
    if (s->si.total && s->decoded != s->si.total)
        return -EBADMSG;
    uint8_t sum[16], zero[16] = { 0 };
    codec_md5_final(&s->md5, sum);
    if (memcmp(s->si.md5, zero, 16) != 0 && memcmp(s->si.md5, sum, 16) != 0)
        return -EBADMSG;
    return 0;
}

int flac_open(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames, void **state)
{
    size_t at = flac_id3_length(data, len);
    if (len < at + 8 + FLAC_STREAMINFO_LEN || memcmp(data + at, "fLaC", 4) != 0)
        return -EINVAL;
    at += 4;
    struct flac_streaminfo si;
    int first = 1, last = 0;
    while (!last) {
        if (at + 4 > len)
            return -EINVAL;
        unsigned type = data[at] & 0x7f;
        size_t n = (size_t)data[at + 1] << 16 | (size_t)data[at + 2] << 8 | data[at + 3];
        last = data[at] >> 7;
        at += 4;
        if (n > len - at || type == 127 || first != (type == 0))
            return -EINVAL;
        if (type == 0 && (n != FLAC_STREAMINFO_LEN || flac_parse_streaminfo(data + at, &si) < 0))
            return -EINVAL;
        first = 0;
        at += n;
    }
    if (si.channels > FLAC_MAX_CHANNELS)
        return -EINVAL;
    struct flac_state *s = calloc(1, sizeof *s);
    if (!s)
        return -ENOMEM;
    s->data = data;
    s->len = len;
    s->pos = at;
    s->si = si;
    codec_md5_init(&s->md5);
    if (grow(s, si.max_block) < 0) {
        flac_close(s);
        return -ENOMEM;
    }
    fmt->rate = (int)si.rate;
    fmt->channels = (int)si.channels;
    fmt->bits = (int)si.bits;
    *frames = si.total ? (long)si.total : -1;
    *state = s;
    return 0;
}

long flac_read(void *state, int32_t *out, long frames)
{
    struct flac_state *s = state;
    long done = 0;
    unsigned ch = s->si.channels;
    while (done < frames) {
        if (s->out_pos < s->out_n) {
            long n = (long)(s->out_n - s->out_pos);
            if (n > frames - done)
                n = frames - done;
            memcpy(out + done * ch, s->out + (size_t)s->out_pos * ch, (size_t)n * ch * sizeof *out);
            s->out_pos += (unsigned)n;
            done += n;
            continue;
        }
        if (s->ended || s->error)
            break;
        int r = decode_frame(s);
        if (r < 0) {
            s->error = r;
        } else if (r == 0) {
            s->ended = 1;
            s->error = finish(s);
        }
    }
    return done ? done : s->error;
}

void flac_close(void *state)
{
    struct flac_state *s = state;
    if (!s)
        return;
    for (unsigned c = 0; c < FLAC_MAX_CHANNELS; c++)
        free(s->ch[c]);
    free(s->out);
    free(s);
}
