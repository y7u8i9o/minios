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

/* Each destination pixel is the average of the source pixels its area
 * covers, weighted by alpha so that transparent pixels do not darken the
 * edges; an enlargement covers less than one source pixel and takes the
 * nearest one. */
struct image *image_scale(const struct image *src, int w, int h)
{
    struct image *dst = image_create(w, h);
    if (!dst)
        return NULL;
    for (int y = 0; y < h; y++) {
        int y0 = (int)((long long)y * src->h / h), y1 = (int)((long long)(y + 1) * src->h / h);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            int x0 = (int)((long long)x * src->w / w), x1 = (int)((long long)(x + 1) * src->w / w);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (x1 - x0 == 1 && y1 - y0 == 1) {
                dst->pixels[(size_t)y * w + x] = src->pixels[(size_t)y0 * src->w + x0];
                continue;
            }
            uint64_t a = 0, r = 0, g = 0, b = 0;
            for (int j = y0; j < y1; j++) {
                const uint32_t *row = src->pixels + (size_t)j * src->w;
                for (int i = x0; i < x1; i++) {
                    uint32_t c = row[i], ca = c >> 24;
                    a += ca;
                    r += ((c >> 16) & 0xff) * ca;
                    g += ((c >> 8) & 0xff) * ca;
                    b += (c & 0xff) * ca;
                }
            }
            uint64_t n = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
            uint32_t out = 0;
            if (a) {
                out = (uint32_t)((a + n / 2) / n) << 24 | (uint32_t)((r + a / 2) / a) << 16 |
                      (uint32_t)((g + a / 2) / a) << 8 | (uint32_t)((b + a / 2) / a);
            }
            dst->pixels[(size_t)y * w + x] = out;
        }
    }
    return dst;
}
