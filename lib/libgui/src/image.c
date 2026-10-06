/* Image allocation and resampling, and the image functions of libgui
 * over the codec library (docs/design/codecs.md): decoding chooses the
 * codec by the content of the data, the PNG and SVG functions name
 * their codec. */
#include <gui/image.h>
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* A decoded picture as a struct image of scale 1, or NULL with errno. */
static struct image *wrap(int err, struct codec_picture *pic)
{
    if (err < 0) {
        errno = -err;
        return NULL;
    }
    struct image *img = malloc(sizeof *img);
    if (!img) {
        codec_picture_free(pic);
        errno = ENOMEM;
        return NULL;
    }
    img->w = pic->w;
    img->h = pic->h;
    img->pixels = pic->pixels;
    img->scale = 1;
    return img;
}

struct image *image_decode(const uint8_t *data, size_t len)
{
    struct codec_picture pic;
    return wrap(codec_image_decode(NULL, data, len, NULL, NULL, &pic), &pic);
}

struct image *image_load(const char *path)
{
    struct codec_picture pic;
    return wrap(codec_image_load(path, NULL, &pic), &pic);
}

struct image *image_render_svg(const char *text, size_t len, int px, uint32_t color)
{
    struct codec_picture pic;
    struct codec_image_request req = { px, px, color };
    const struct codec *svg = codec_find("svg");
    if (!svg) {
        errno = ENOTSUP;
        return NULL;
    }
    return wrap(codec_image_decode(svg, (const uint8_t *)text, len, NULL, &req, &pic), &pic);
}

struct image *image_load_svg(const char *path, int px, uint32_t color)
{
    uint8_t *data;
    size_t len;
    int err = codec_read_file(path, &data, &len);
    if (err < 0) {
        errno = -err;
        return NULL;
    }
    struct image *img = image_render_svg((const char *)data, len, px, color);
    free(data);
    return img;
}

long image_encode_png(const struct image *img, uint8_t **data)
{
    if (!img)
        return -EINVAL;
    struct codec_picture pic = { img->w, img->h, img->pixels };
    return codec_image_encode(codec_find("png"), &pic, data);
}

int image_save_png(const struct image *img, const char *path)
{
    if (!img)
        return -EINVAL;
    struct codec_picture pic = { img->w, img->h, img->pixels };
    return codec_image_save(&pic, path, "png");
}

long zlib_inflate(uint8_t *dst, size_t cap, const uint8_t *src, size_t len)
{
    return codec_inflate(dst, cap, src, len);
}

void image_free(struct image *img)
{
    if (img) {
        free(img->pixels);
        free(img);
    }
}

struct image *image_create(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        errno = EINVAL;
        return NULL;
    }
    struct image *img = malloc(sizeof *img);
    if (!img || !(img->pixels = calloc((size_t)w * h, 4))) {
        free(img);
        errno = ENOMEM;
        return NULL;
    }
    img->w = w;
    img->h = h;
    img->scale = 1;
    return img;
}

/* image_scale resamples each axis separately with premultiplied alpha, so
 * that transparent pixels do not darken the edges. An axis that becomes
 * shorter averages the source pixels that each destination pixel covers,
 * weighted by the covered length. An axis that becomes longer
 * interpolates linearly between the two nearest source pixels, measured
 * from the pixel centres. The function works on one destination row at a
 * time and resamples each source row horizontally into a cache. A large
 * image therefore needs no temporary copy of its own size.
 *
 * A row of the cache contains four channels per pixel: alpha, red, green
 * and blue, premultiplied and multiplied by 256. */
#define CH 4

static void premultiply(uint32_t c, uint32_t out[CH])
{
    uint32_t a = c >> 24;
    out[0] = a * 256;
    out[1] = ((c >> 16) & 0xff) * a * 256 / 255;
    out[2] = ((c >> 8) & 0xff) * a * 256 / 255;
    out[3] = (c & 0xff) * a * 256 / 255;
}

/* Resample one source row of sw pixels to dw pixels into out. */
static void resample_row(const uint32_t *src, int sw, uint32_t *out, int dw)
{
    if (dw <= sw) {
        /* Destination pixel d covers [d * sw, (d + 1) * sw) and source
         * pixel i covers [i * dw, (i + 1) * dw), both in units of 1 / dw
         * source pixel. */
        for (int d = 0; d < dw; d++) {
            uint64_t lo = (uint64_t)d * (uint64_t)sw, hi = lo + (uint64_t)sw;
            uint64_t acc[CH] = { 0, 0, 0, 0 };
            for (int i = (int)(lo / (uint64_t)dw); (uint64_t)i * (uint64_t)dw < hi && i < sw; i++) {
                uint64_t a = (uint64_t)i * (uint64_t)dw, b = a + (uint64_t)dw;
                uint64_t weight = (b < hi ? b : hi) - (a > lo ? a : lo);
                uint32_t px[CH];
                premultiply(src[i], px);
                for (int k = 0; k < CH; k++)
                    acc[k] += px[k] * weight;
            }
            for (int k = 0; k < CH; k++)
                out[d * CH + k] = (uint32_t)((acc[k] + (uint64_t)sw / 2) / (uint64_t)sw);
        }
        return;
    }
    for (int d = 0; d < dw; d++) {
        /* The centre of d in source pixels, in 16.16 fixed point. */
        int64_t pos = ((int64_t)(2 * d + 1) * sw * 65536) / (2 * (int64_t)dw) - 32768;
        if (pos < 0)
            pos = 0;
        int i0 = (int)(pos >> 16), i1 = i0 + 1 < sw ? i0 + 1 : sw - 1;
        uint64_t f = (uint64_t)(pos & 0xffff);
        uint32_t p0[CH], p1[CH];
        premultiply(src[i0], p0);
        premultiply(src[i1], p1);
        for (int k = 0; k < CH; k++)
            out[d * CH + k] = (uint32_t)((p0[k] * (65536 - f) + p1[k] * f + 32768) >> 16);
    }
}

/* The row cache: the horizontally resampled source rows with the indices
 * in index, -1 when a slot is empty. */
struct row_cache {
    uint32_t *rows[2];
    int index[2];
    int next;
};

static const uint32_t *cached_row(struct row_cache *c, const struct image *src, int y, int dw)
{
    for (int k = 0; k < 2; k++)
        if (c->index[k] == y)
            return c->rows[k];
    int k = c->next;
    c->next ^= 1;
    resample_row(src->pixels + (size_t)y * src->w, src->w, c->rows[k], dw);
    c->index[k] = y;
    return c->rows[k];
}

static uint32_t unpremultiply(const uint64_t v[CH])
{
    uint64_t a = v[0];
    uint32_t alpha = (uint32_t)((a + 128) / 256);
    if (alpha == 0)
        return 0;
    if (alpha > 255)
        alpha = 255;
    uint32_t out = alpha << 24;
    for (int k = 1; k < CH; k++) {
        uint64_t c = (v[k] * 255 + a / 2) / a;
        out |= (uint32_t)(c > 255 ? 255 : c) << (8 * (CH - 1 - k));
    }
    return out;
}

struct image *image_scale(const struct image *src, int w, int h)
{
    struct image *dst = image_create(w, h);
    if (!dst)
        return NULL;
    struct row_cache cache = { { NULL, NULL }, { -1, -1 }, 0 };
    cache.rows[0] = malloc((size_t)w * CH * sizeof(uint32_t));
    cache.rows[1] = malloc((size_t)w * CH * sizeof(uint32_t));
    /* The sum of the weighted rows of one destination row (reduction). */
    uint64_t *acc = malloc((size_t)w * CH * sizeof(uint64_t));
    if (!cache.rows[0] || !cache.rows[1] || !acc) {
        free(cache.rows[0]);
        free(cache.rows[1]);
        free(acc);
        image_free(dst);
        errno = ENOMEM;
        return NULL;
    }
    int sh = src->h;
    for (int y = 0; y < h; y++) {
        uint32_t *out = dst->pixels + (size_t)y * w;
        if (h <= sh) {
            /* Rows y * sh to (y + 1) * sh in units of 1 / h source row. */
            uint64_t lo = (uint64_t)y * (uint64_t)sh, hi = lo + (uint64_t)sh;
            memset(acc, 0, (size_t)w * CH * sizeof(uint64_t));
            for (int j = (int)(lo / (uint64_t)h); (uint64_t)j * (uint64_t)h < hi && j < sh; j++) {
                uint64_t a = (uint64_t)j * (uint64_t)h, b = a + (uint64_t)h;
                uint64_t weight = (b < hi ? b : hi) - (a > lo ? a : lo);
                const uint32_t *row = cached_row(&cache, src, j, w);
                for (int x = 0; x < w * CH; x++)
                    acc[x] += (uint64_t)row[x] * weight;
            }
            for (int x = 0; x < w; x++) {
                uint64_t v[CH];
                for (int k = 0; k < CH; k++)
                    v[k] = (acc[x * CH + k] + (uint64_t)sh / 2) / (uint64_t)sh;
                out[x] = unpremultiply(v);
            }
            continue;
        }
        int64_t pos = ((int64_t)(2 * y + 1) * sh * 65536) / (2 * (int64_t)h) - 32768;
        if (pos < 0)
            pos = 0;
        int j0 = (int)(pos >> 16), j1 = j0 + 1 < sh ? j0 + 1 : sh - 1;
        uint64_t f = (uint64_t)(pos & 0xffff);
        const uint32_t *r0 = cached_row(&cache, src, j0, w);
        const uint32_t *r1 = cached_row(&cache, src, j1, w);
        for (int x = 0; x < w; x++) {
            uint64_t v[CH];
            for (int k = 0; k < CH; k++)
                v[k] = ((uint64_t)r0[x * CH + k] * (65536 - f) + (uint64_t)r1[x * CH + k] * f + 32768) >> 16;
            out[x] = unpremultiply(v);
        }
    }
    free(cache.rows[0]);
    free(cache.rows[1]);
    free(acc);
    return dst;
}
