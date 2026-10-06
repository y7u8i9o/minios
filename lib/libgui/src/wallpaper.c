/* The background of the desktop and of the greeter (gui/wallpaper.h). */
#include <gui/wallpaper.h>
#include <errno.h>
#include <string.h>

enum wallpaper_mode wallpaper_mode_parse(const char *text)
{
    if (strcmp(text, "center") == 0)
        return WALLPAPER_CENTER;
    if (strcmp(text, "tile") == 0)
        return WALLPAPER_TILE;
    if (strcmp(text, "stretch") == 0)
        return WALLPAPER_STRETCH;
    return WALLPAPER_FILL;
}

/* Draw src with its alpha over the opaque dst at x, y, clipped to dst. */
static void draw_over(struct image *dst, int x, int y, const struct image *src)
{
    for (int j = 0; j < src->h; j++) {
        int py = y + j;
        if (py < 0 || py >= dst->h)
            continue;
        const uint32_t *from = src->pixels + (size_t)j * src->w;
        uint32_t *row = dst->pixels + (size_t)py * dst->w;
        for (int i = 0; i < src->w; i++) {
            int px = x + i;
            if (px < 0 || px >= dst->w)
                continue;
            uint32_t c = from[i], a = c >> 24, d = row[px];
            if (a == 255) {
                row[px] = c;
                continue;
            }
            uint32_t r = ((d >> 16 & 0xff) * (255 - a) + (c >> 16 & 0xff) * a) / 255;
            uint32_t g = ((d >> 8 & 0xff) * (255 - a) + (c >> 8 & 0xff) * a) / 255;
            uint32_t b = ((d & 0xff) * (255 - a) + (c & 0xff) * a) / 255;
            row[px] = 0xff000000u | r << 16 | g << 8 | b;
        }
    }
}

struct image *wallpaper_render(const struct image *img, enum wallpaper_mode mode, uint32_t color, int w, int h,
                               int scale)
{
    if (scale < 1)
        scale = 1;
    int dw = w * scale, dh = h * scale;
    struct image *out = image_create(dw, dh);
    if (!out)
        return NULL;
    out->scale = scale;
    for (size_t i = 0; i < (size_t)dw * dh; i++)
        out->pixels[i] = 0xff000000u | (color & 0x00ffffff);
    if (!img || img->w <= 0 || img->h <= 0)
        return out;
    /* The size of img on the device in the mode. */
    long iw = img->w, ih = img->h;
    long tw = (long)image_lw(img) * scale, th = (long)image_lh(img) * scale;
    if (mode == WALLPAPER_FILL) {
        /* The larger of the two factors covers the area. */
        if ((long long)dw * ih >= (long long)dh * iw) {
            tw = dw;
            th = (long)(((long long)ih * dw + iw / 2) / iw);
        } else {
            th = dh;
            tw = (long)(((long long)iw * dh + ih / 2) / ih);
        }
    } else if (mode == WALLPAPER_STRETCH) {
        tw = dw;
        th = dh;
    }
    if (tw < 1 || th < 1 || tw > 16384 || th > 16384) {
        image_free(out);
        errno = EINVAL;
        return NULL;
    }
    struct image *scaled = NULL;
    if (tw != iw || th != ih) {
        scaled = image_scale(img, (int)tw, (int)th);
        if (!scaled) {
            image_free(out);
            return NULL;
        }
    }
    const struct image *use = scaled ? scaled : img;
    if (mode == WALLPAPER_TILE) {
        for (int y = 0; y < dh; y += use->h)
            for (int x = 0; x < dw; x += use->w)
                draw_over(out, x, y, use);
    } else {
        draw_over(out, (int)((dw - use->w) / 2), (int)((dh - use->h) / 2), use);
    }
    image_free(scaled);
    return out;
}
