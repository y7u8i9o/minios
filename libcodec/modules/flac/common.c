/* The parts of the FLAC module shared by the decoder and the encoder. */
#include "flac.h"

const unsigned flac_block_sizes[16] = { 0,   192,  576,  1152, 2304, 4608,  0,     0,
                                        256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
const unsigned flac_rates[16] = { 0,     88200, 176400, 192000, 8000, 16000, 22050, 24000,
                                  32000, 44100, 48000,  96000,  0,    0,     0,     0 };
const unsigned flac_sample_sizes[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

/* ---- reading ---- */

uint64_t flac_bits(struct flac_reader *r, unsigned n)
{
    if (!n)
        return 0;
    if (r->pos + n > r->len * 8) {
        r->failed = 1;
        r->pos = r->len * 8;
        return 0;
    }
    size_t byte = r->pos >> 3;
    unsigned off = (unsigned)(r->pos & 7);
    uint64_t w = 0;
    for (int i = 0; i < 8; i++)
        w = w << 8 | (byte + (size_t)i < r->len ? r->p[byte + (size_t)i] : 0);
    r->pos += n;
    return (w << off) >> (64 - n);
}

int64_t flac_sbits(struct flac_reader *r, unsigned n)
{
    uint64_t v = flac_bits(r, n);
    if (n && (v >> (n - 1)) & 1)
        return (int64_t)(v - (1ull << n));
    return (int64_t)v;
}

uint32_t flac_unary(struct flac_reader *r)
{
    uint32_t zeros = 0;
    for (;;) {
        if (r->pos >= r->len * 8) {
            r->failed = 1;
            return zeros;
        }
        unsigned off = (unsigned)(r->pos & 7);
        unsigned b = (unsigned)(uint8_t)(r->p[r->pos >> 3] << off);
        if (b) {
            unsigned lead = (unsigned)__builtin_clz(b) - 24;
            r->pos += lead + 1;
            return zeros + lead;
        }
        zeros += 8 - off;
        r->pos += 8 - off;
    }
}

void flac_align(struct flac_reader *r)
{
    r->pos = (r->pos + 7) & ~(size_t)7;
}

/* ---- writing ---- */

static void put_byte(struct flac_writer *w, uint8_t b)
{
    if (w->len == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 65536;
        uint8_t *grown = realloc(w->data, cap);
        if (!grown) {
            w->failed = 1;
            return;
        }
        w->data = grown;
        w->cap = cap;
    }
    w->data[w->len++] = b;
}

void flac_put(struct flac_writer *w, uint64_t v, unsigned n)
{
    if (n > 32) {
        flac_put(w, v >> 32, n - 32);
        n = 32;
    }
    if (!n)
        return;
    w->acc = w->acc << n | (v & ((1ull << n) - 1));
    w->nacc += n;
    while (w->nacc >= 8) {
        w->nacc -= 8;
        put_byte(w, (uint8_t)(w->acc >> w->nacc));
    }
}

void flac_put_signed(struct flac_writer *w, int64_t v, unsigned n)
{
    flac_put(w, (uint64_t)v, n);
}

void flac_put_unary(struct flac_writer *w, uint32_t zeros)
{
    for (; zeros >= 32; zeros -= 32)
        flac_put(w, 0, 32);
    flac_put(w, 1, zeros + 1);
}

void flac_put_align(struct flac_writer *w)
{
    if (w->nacc)
        flac_put(w, 0, 8 - w->nacc);
}

/* ---- checksums ---- */

uint8_t flac_crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (uint8_t)(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

static uint16_t crc16_table[256];
static int crc16_ready;

uint16_t flac_crc16(const uint8_t *p, size_t n)
{
    /* The table is computed on first use. Two threads that compute it at
     * the same time write the same values. */
    if (!__atomic_load_n(&crc16_ready, __ATOMIC_ACQUIRE)) {
        for (unsigned i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)(i << 8);
            for (int b = 0; b < 8; b++)
                c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
            crc16_table[i] = c;
        }
        __atomic_store_n(&crc16_ready, 1, __ATOMIC_RELEASE);
    }
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++)
        crc = (uint16_t)(crc << 8) ^ crc16_table[(crc >> 8) ^ p[i]];
    return crc;
}

/* ---- metadata ---- */

int flac_parse_streaminfo(const uint8_t *p, struct flac_streaminfo *si)
{
    struct flac_reader r = { p, FLAC_STREAMINFO_LEN, 0, 0 };
    si->min_block = (unsigned)flac_bits(&r, 16);
    si->max_block = (unsigned)flac_bits(&r, 16);
    si->min_frame = (uint32_t)flac_bits(&r, 24);
    si->max_frame = (uint32_t)flac_bits(&r, 24);
    si->rate = (unsigned)flac_bits(&r, 20);
    si->channels = (unsigned)flac_bits(&r, 3) + 1;
    si->bits = (unsigned)flac_bits(&r, 5) + 1;
    si->total = flac_bits(&r, 36);
    memcpy(si->md5, p + 18, 16);
    if (si->max_block < 16 || si->max_block > FLAC_MAX_BLOCK || si->rate == 0 || si->bits < 4)
        return -EINVAL;
    return 0;
}

size_t flac_id3_length(const uint8_t *d, size_t len)
{
    if (len < 10 || memcmp(d, "ID3", 3) != 0 || d[3] == 0xff || d[4] == 0xff ||
        ((d[6] | d[7] | d[8] | d[9]) & 0x80))
        return 0;
    size_t size = (size_t)d[6] << 21 | (size_t)d[7] << 14 | (size_t)d[8] << 7 | d[9];
    size += 10 + (d[5] & 0x10 ? 10 : 0);
    return size <= len ? size : 0;
}

void flac_md5_samples(struct codec_md5 *m, const int64_t *const *ch, unsigned channels, unsigned n,
                      unsigned bits)
{
    unsigned bytes = (bits + 7) / 8;
    uint8_t buf[4096];
    size_t used = 0;
    for (unsigned i = 0; i < n; i++)
        for (unsigned c = 0; c < channels; c++) {
            if (used + bytes > sizeof buf) {
                codec_md5_update(m, buf, used);
                used = 0;
            }
            uint64_t v = (uint64_t)ch[c][i];
            for (unsigned b = 0; b < bytes; b++)
                buf[used++] = (uint8_t)(v >> (8 * b));
        }
    codec_md5_update(m, buf, used);
}
