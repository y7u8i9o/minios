/* PNG encoding: RGB or RGBA at 8 bits, one adaptive filter per row, and a
 * zlib stream of a single deflate block with the fixed Huffman codes
 * (RFC 1951 3.2.6) over LZ77 matches found with hash chains. */
#include "png.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <minios/crc32.h>

/* ---- output buffer ---- */

struct out {
    uint8_t *data;
    size_t len, cap;
    int failed;
    uint32_t bits;              /* pending bits, least significant first */
    int nbits;
};

static void put_byte(struct out *o, uint8_t b)
{
    if (o->len == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 65536;
        uint8_t *grown = o->failed ? NULL : realloc(o->data, cap);
        if (!grown) {
            o->failed = 1;
            return;
        }
        o->data = grown;
        o->cap = cap;
    }
    o->data[o->len++] = b;
}

static void put32(struct out *o, uint32_t v)
{
    put_byte(o, (uint8_t)(v >> 24));
    put_byte(o, (uint8_t)(v >> 16));
    put_byte(o, (uint8_t)(v >> 8));
    put_byte(o, (uint8_t)v);
}

/* n bits of v, least significant first (deflate's order for every field
 * except the Huffman codes). */
static void put_bits(struct out *o, uint32_t v, int n)
{
    o->bits |= v << o->nbits;
    o->nbits += n;
    while (o->nbits >= 8) {
        put_byte(o, (uint8_t)o->bits);
        o->bits >>= 8;
        o->nbits -= 8;
    }
}

static void flush_bits(struct out *o)
{
    if (o->nbits > 0)
        put_byte(o, (uint8_t)o->bits);
    o->bits = 0;
    o->nbits = 0;
}

/* A Huffman code of n bits, most significant bit first. */
static void put_code(struct out *o, uint32_t code, int n)
{
    uint32_t rev = 0;
    for (int i = 0; i < n; i++)
        rev |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(o, rev, n);
}

/* ---- checksums ---- */

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n > 0) {
        size_t chunk = n < 5552 ? n : 5552;     /* no overflow before the modulo */
        n -= chunk;
        while (chunk--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

/* ---- deflate with the fixed codes ---- */

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* Literal and length symbols 0 to 287 in the fixed code. */
static void put_symbol(struct out *o, int sym)
{
    if (sym < 144)
        put_code(o, 0x30 + (uint32_t)sym, 8);
    else if (sym < 256)
        put_code(o, 0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280)
        put_code(o, (uint32_t)(sym - 256), 7);
    else
        put_code(o, 0xc0 + (uint32_t)(sym - 280), 8);
}

static void put_match(struct out *o, int len, int dist)
{
    int i = 28;
    while (len_base[i] > len)
        i--;
    put_symbol(o, 257 + i);
    put_bits(o, (uint32_t)(len - len_base[i]), len_extra[i]);
    int d = 29;
    while (dist_base[d] > dist)
        d--;
    put_code(o, (uint32_t)d, 5);
    put_bits(o, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
}

#define WINDOW 32768
#define HASH_BITS 15
#define MAX_CHAIN 48
#define MIN_MATCH 3
#define MAX_MATCH 258

static uint32_t hash3(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16 ^ (uint32_t)p[1] << 8 ^ p[2]) * 2654435761u >> (32 - HASH_BITS);
}

/* Greedy LZ77: every position is inserted into its hash chain; a match
 * is the longest of the first MAX_CHAIN candidates within the window. */
static int deflate_fixed(struct out *o, const uint8_t *src, size_t n)
{
    int32_t *head = malloc(sizeof(int32_t) << HASH_BITS);
    int32_t *prev = malloc(sizeof(int32_t) * WINDOW);
    if (!head || !prev) {
        free(head);
        free(prev);
        return -ENOMEM;
    }
    for (size_t i = 0; i < (size_t)1 << HASH_BITS; i++)
        head[i] = -1;
    put_bits(o, 1, 1);          /* BFINAL */
    put_bits(o, 1, 2);          /* BTYPE 01: fixed codes */
    size_t pos = 0;
    while (pos < n) {
        int best = 0;
        size_t best_dist = 0;
        if (pos + MIN_MATCH <= n) {
            uint32_t h = hash3(src + pos);
            size_t limit = n - pos < MAX_MATCH ? n - pos : MAX_MATCH;
            int32_t cand = head[h];
            for (int chain = 0; cand >= 0 && chain < MAX_CHAIN; chain++) {
                size_t dist = pos - (size_t)cand;
                if (dist > WINDOW - 1)
                    break;
                if (src[cand + best] == src[pos + best]) {
                    size_t l = 0;
                    while (l < limit && src[cand + l] == src[pos + l])
                        l++;
                    if ((int)l > best) {
                        best = (int)l;
                        best_dist = dist;
                        if (l == limit)
                            break;
                    }
                }
                int32_t next = prev[cand % WINDOW];
                if (next >= cand)
                    break;
                cand = next;
            }
        }
        size_t step = best >= MIN_MATCH ? (size_t)best : 1;
        if (best >= MIN_MATCH)
            put_match(o, best, (int)best_dist);
        else
            put_symbol(o, src[pos]);
        for (size_t k = 0; k < step; k++, pos++) {
            if (pos + MIN_MATCH <= n) {
                uint32_t h = hash3(src + pos);
                prev[pos % WINDOW] = head[h];
                head[h] = (int32_t)pos;
            }
        }
    }
    put_symbol(o, 256);
    flush_bits(o);
    free(head);
    free(prev);
    return 0;
}

/* ---- filters ---- */

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Filter one row with type f into out; returns the sum of the filtered
 * bytes read as signed values, the usual estimate of how well a row
 * compresses. */
static unsigned long filter_row(int f, const uint8_t *row, const uint8_t *prev, size_t stride, int bpp, uint8_t *out)
{
    unsigned long cost = 0;
    for (size_t i = 0; i < stride; i++) {
        int a = i >= (size_t)bpp ? row[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;
        int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : paeth(a, b, c);
        uint8_t v = (uint8_t)(row[i] - pred);
        out[i] = v;
        cost += v < 128 ? v : 256 - v;
    }
    return cost;
}

/* ---- PNG ---- */

static void put_chunk(struct out *o, const char *type, const uint8_t *body, size_t n)
{
    put32(o, (uint32_t)n);
    size_t start = o->len;
    for (int i = 0; i < 4; i++)
        put_byte(o, (uint8_t)type[i]);
    for (size_t i = 0; i < n; i++)
        put_byte(o, body[i]);
    if (o->failed)
        return;
    put32(o, crc32(0, o->data + start, n + 4));
}

long png_encode(const struct codec_picture *img, uint8_t **result)
{
    if (!img || img->w <= 0 || img->h <= 0)
        return -EINVAL;
    int alpha = 0;
    size_t npix = (size_t)img->w * img->h;
    for (size_t i = 0; i < npix && !alpha; i++)
        alpha = (img->pixels[i] >> 24) != 0xff;
    int bpp = alpha ? 4 : 3;
    size_t stride = (size_t)img->w * bpp;
    uint8_t *raw = malloc((stride + 1) * (size_t)img->h);
    uint8_t *rows = malloc(stride * 2), *trial = malloc(stride);
    if (!raw || !rows || !trial) {
        free(raw);
        free(rows);
        free(trial);
        return -ENOMEM;
    }
    uint8_t *cur = rows, *prev = NULL;
    for (int y = 0; y < img->h; y++) {
        const uint32_t *px = img->pixels + (size_t)y * img->w;
        for (int x = 0; x < img->w; x++) {
            uint8_t *d = cur + (size_t)x * bpp;
            d[0] = (uint8_t)(px[x] >> 16);
            d[1] = (uint8_t)(px[x] >> 8);
            d[2] = (uint8_t)px[x];
            if (alpha)
                d[3] = (uint8_t)(px[x] >> 24);
        }
        uint8_t *line = raw + (size_t)y * (stride + 1);
        unsigned long best = (unsigned long)-1;
        for (int f = 0; f < 5; f++) {
            unsigned long cost = filter_row(f, cur, prev, stride, bpp, trial);
            if (cost < best) {
                best = cost;
                line[0] = (uint8_t)f;
                memcpy(line + 1, trial, stride);
            }
        }
        prev = cur;
        cur = cur == rows ? rows + stride : rows;
    }
    free(rows);
    free(trial);

    struct out z = { 0 };
    put_byte(&z, 0x78);         /* deflate, 32 KiB window */
    put_byte(&z, 0x9c);
    size_t rawlen = (stride + 1) * (size_t)img->h;
    int err = deflate_fixed(&z, raw, rawlen);
    put32(&z, adler32(raw, rawlen));
    free(raw);
    if (err < 0 || z.failed) {
        free(z.data);
        return err < 0 ? err : -ENOMEM;
    }

    struct out o = { 0 };
    for (int i = 0; i < 8; i++)
        put_byte(&o, png_signature[i]);
    uint8_t ihdr[13] = {
        (uint8_t)(img->w >> 24), (uint8_t)(img->w >> 16), (uint8_t)(img->w >> 8), (uint8_t)img->w,
        (uint8_t)(img->h >> 24), (uint8_t)(img->h >> 16), (uint8_t)(img->h >> 8), (uint8_t)img->h,
        8, (uint8_t)(alpha ? 6 : 2), 0, 0, 0,
    };
    put_chunk(&o, "IHDR", ihdr, sizeof ihdr);
    put_chunk(&o, "IDAT", z.data, z.len);
    put_chunk(&o, "IEND", NULL, 0);
    free(z.data);
    if (o.failed) {
        free(o.data);
        return -ENOMEM;
    }
    *result = o.data;
    return (long)o.len;
}
