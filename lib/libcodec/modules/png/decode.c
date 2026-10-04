/* PNG decoding: chunks, zlib inflate, the five filters, Adam7
 * interlacing, and every colour type at every bit depth the format
 * allows (1, 2, 4, 8 and 16 bits); 16 bit samples are rounded to 8. */
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Undo the row filters of one (sub)image of h rows of stride bytes; bpp
 * is the filter distance, the bytes per complete pixel rounded up to 1. */
static int unfilter(uint8_t *raw, size_t stride, int h, int bpp)
{
    uint8_t *prev = NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1);
        int f = row[0];
        uint8_t *d = row + 1;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? d[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: d[i] += a; break;
            case 2: d[i] += b; break;
            case 3: d[i] += (a + b) / 2; break;
            case 4: d[i] += paeth(a, b, c); break;
            default: return -1;
            }
        }
        prev = d;
    }
    return 0;
}

/* The seven passes of Adam7: first column, first row, column step, row
 * step. A file without interlace is one pass of the whole image. */
static const int adam7[7][4] = {
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 },
};
static const int single_pass[1][4] = { { 0, 0, 1, 1 } };

static size_t row_bytes(int w, int bits)
{
    return ((size_t)w * bits + 7) / 8;
}

/* Sample i of pixel x in an unfiltered row, without scaling: 0 to
 * 2^depth - 1. */
static unsigned sample(const uint8_t *row, int x, int i, int channels, int depth)
{
    if (depth == 8)
        return row[(size_t)x * channels + i];
    if (depth == 16) {
        const uint8_t *p = row + ((size_t)x * channels + i) * 2;
        return (unsigned)p[0] << 8 | p[1];
    }
    size_t bit = (size_t)x * depth;      /* one channel below 8 bits */
    return (unsigned)(row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
}

/* A sample scaled to 8 bits. */
static uint32_t to8(unsigned v, int depth)
{
    if (depth == 8)
        return v;
    if (depth == 16)
        return (v * 255 + 32767) / 65535;
    return v * 255 / ((1u << depth) - 1);
}

static int depth_allowed(int ctype, int depth)
{
    switch (ctype) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2: case 4: case 6: return depth == 8 || depth == 16;
    default: return 0;
    }
}

int png_decode(const uint8_t *data, size_t len, const struct codec_image_request *req, struct codec_picture *img)
{
    if (len < 33 || memcmp(data, png_signature, 8) != 0)
        return -EINVAL;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t palette[256][4];
    int npal = 0;
    memset(palette, 0, sizeof palette);
    for (int i = 0; i < 256; i++)
        palette[i][3] = 255;
    /* Transparent colour of grey and RGB images, compared with the
     * unscaled samples. */
    long trns_grey = -1, trns_rgb[3] = { -1, -1, -1 };
    uint8_t *idat = NULL;
    size_t idat_len = 0;
    size_t pos = 8;
    while (pos + 12 <= len) {
        uint32_t clen = rd32(data + pos);
        const uint8_t *type = data + pos + 4, *body = data + pos + 8;
        if (clen > len - pos - 12)
            goto bad;
        if (memcmp(type, "IHDR", 4) == 0 && clen == 13) {
            w = (int)rd32(body);
            h = (int)rd32(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            npal = (int)(clen / 3);
            if (npal > 256)
                goto bad;
            for (int i = 0; i < npal; i++) {
                palette[i][0] = body[i * 3];
                palette[i][1] = body[i * 3 + 1];
                palette[i][2] = body[i * 3 + 2];
            }
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (ctype == 3) {
                for (uint32_t i = 0; i < clen && i < 256; i++)
                    palette[i][3] = body[i];
            } else if (ctype == 0 && clen >= 2) {
                trns_grey = body[0] << 8 | body[1];
            } else if (ctype == 2 && clen >= 6) {
                for (int i = 0; i < 3; i++)
                    trns_rgb[i] = body[i * 2] << 8 | body[i * 2 + 1];
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            uint8_t *grown = realloc(idat, idat_len + clen);
            if (!grown)
                goto nomem;
            idat = grown;
            memcpy(idat + idat_len, body, clen);
            idat_len += clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !depth_allowed(ctype, depth) || interlace > 1 || !idat)
        goto bad;
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    int bits = channels * depth, bpp = bits >= 8 ? bits / 8 : 1;
    const int (*passes)[4] = interlace ? adam7 : single_pass;
    int npasses = interlace ? 7 : 1;
    /* The filtered data of all passes, one after the other; an empty
     * pass has no rows. */
    size_t rawlen = 0;
    for (int p = 0; p < npasses; p++) {
        int pw = (w - passes[p][0] + passes[p][2] - 1) / passes[p][2];
        int ph = (h - passes[p][1] + passes[p][3] - 1) / passes[p][3];
        if (pw > 0 && ph > 0)
            rawlen += (row_bytes(pw, bits) + 1) * (size_t)ph;
    }
    uint8_t *raw = malloc(rawlen);
    if (!raw)
        goto nomem;
    long got = codec_inflate(raw, rawlen, idat, idat_len);
    free(idat);
    idat = NULL;
    if (got != (long)rawlen) {
        free(raw);
        goto bad;
    }
    if (!(img->pixels = malloc((size_t)w * h * 4))) {
        free(raw);
        goto nomem;
    }
    img->w = w;
    img->h = h;
    uint8_t *pass = raw;
    for (int p = 0; p < npasses; p++) {
        int x0 = passes[p][0], y0 = passes[p][1], dx = passes[p][2], dy = passes[p][3];
        int pw = (w - x0 + dx - 1) / dx, ph = (h - y0 + dy - 1) / dy;
        if (pw <= 0 || ph <= 0)
            continue;
        size_t stride = row_bytes(pw, bits);
        if (unfilter(pass, stride, ph, bpp) < 0) {
            free(raw);
            codec_picture_free(img);
            goto bad;
        }
        for (int j = 0; j < ph; j++) {
            const uint8_t *row = pass + (size_t)j * (stride + 1) + 1;
            uint32_t *out = img->pixels + (size_t)(y0 + j * dy) * w;
            for (int i = 0; i < pw; i++) {
                uint32_t r, g, b, a = 255;
                unsigned s0 = sample(row, i, 0, channels, depth);
                switch (ctype) {
                case 0:
                    r = g = b = to8(s0, depth);
                    if ((long)s0 == trns_grey)
                        a = 0;
                    break;
                case 2: {
                    unsigned s1 = sample(row, i, 1, channels, depth), s2 = sample(row, i, 2, channels, depth);
                    r = to8(s0, depth);
                    g = to8(s1, depth);
                    b = to8(s2, depth);
                    if ((long)s0 == trns_rgb[0] && (long)s1 == trns_rgb[1] && (long)s2 == trns_rgb[2])
                        a = 0;
                    break;
                }
                case 3:
                    r = palette[s0][0];
                    g = palette[s0][1];
                    b = palette[s0][2];
                    a = palette[s0][3];
                    break;
                case 4:
                    r = g = b = to8(s0, depth);
                    a = to8(sample(row, i, 1, channels, depth), depth);
                    break;
                default:
                    r = to8(s0, depth);
                    g = to8(sample(row, i, 1, channels, depth), depth);
                    b = to8(sample(row, i, 2, channels, depth), depth);
                    a = to8(sample(row, i, 3, channels, depth), depth);
                    break;
                }
                out[x0 + i * dx] = a << 24 | r << 16 | g << 8 | b;
            }
        }
        pass += (stride + 1) * (size_t)ph;
    }
    free(raw);
    return 0;
bad:
    free(idat);
    return -EINVAL;
nomem:
    free(idat);
    return -ENOMEM;
}
