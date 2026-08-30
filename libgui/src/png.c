/* PNG decoding: chunks, zlib inflate, the five filters, colour types
 * grey, RGB, palette, grey with alpha and RGBA at 8 bits, no interlace. */
#include <gui/image.h>
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

static int unfilter(uint8_t *raw, int w, int h, int bpp)
{
    size_t stride = (size_t)w * bpp;
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

struct image *image_decode(const uint8_t *data, size_t len)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (len < 33 || memcmp(data, sig, 8) != 0) {
        errno = EINVAL;
        return NULL;
    }
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t palette[256][4];
    int npal = 0;
    for (int i = 0; i < 256; i++)
        palette[i][3] = 255;
    int trns_grey = -1, trns_rgb[3] = { -1, -1, -1 };
    uint8_t *idat = NULL;
    size_t idat_len = 0;
    size_t pos = 8;
    struct image *img = NULL;
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
                trns_grey = body[1];
            } else if (ctype == 2 && clen >= 6) {
                trns_rgb[0] = body[1];
                trns_rgb[1] = body[3];
                trns_rgb[2] = body[5];
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
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || depth != 8 || interlace != 0 || !idat)
        goto bad;
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!channels)
        goto bad;
    size_t stride = (size_t)w * channels, rawlen = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(rawlen);
    if (!raw)
        goto nomem;
    long got = zlib_inflate(raw, rawlen, idat, idat_len);
    free(idat);
    idat = NULL;
    if (got != (long)rawlen || unfilter(raw, w, h, channels) < 0) {
        free(raw);
        goto bad;
    }
    img = malloc(sizeof *img);
    if (!img || !(img->pixels = malloc((size_t)w * h * 4))) {
        free(img);
        free(raw);
        goto nomem;
    }
    img->w = w;
    img->h = h;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = raw + (size_t)y * (stride + 1) + 1;
        for (int x = 0; x < w; x++) {
            const uint8_t *px = row + (size_t)x * channels;
            uint32_t r, g, b, a = 255;
            switch (ctype) {
            case 0: r = g = b = px[0]; if (px[0] == trns_grey) a = 0; break;
            case 2:
                r = px[0];
                g = px[1];
                b = px[2];
                if (px[0] == trns_rgb[0] && px[1] == trns_rgb[1] && px[2] == trns_rgb[2])
                    a = 0;
                break;
            case 3: r = palette[px[0]][0]; g = palette[px[0]][1]; b = palette[px[0]][2]; a = palette[px[0]][3]; break;
            case 4: r = g = b = px[0]; a = px[1]; break;
            default: r = px[0]; g = px[1]; b = px[2]; a = px[3]; break;
            }
            img->pixels[(size_t)y * w + x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    free(raw);
    return img;
bad:
    free(idat);
    errno = EINVAL;
    return NULL;
nomem:
    free(idat);
    errno = ENOMEM;
    return NULL;
}

struct image *image_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t cap = 4096, n = 0;
    uint8_t *data = malloc(cap);
    while (data) {
        n += fread(data + n, 1, cap - n, f);
        if (n < cap)
            break;
        cap *= 2;
        data = realloc(data, cap);
    }
    fclose(f);
    if (!data) {
        errno = ENOMEM;
        return NULL;
    }
    struct image *img = image_decode(data, n);
    free(data);
    return img;
}

void image_free(struct image *img)
{
    if (img) {
        free(img->pixels);
        free(img);
    }
}
