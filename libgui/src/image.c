/* Image allocation and resampling. */
#include <gui/image.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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
