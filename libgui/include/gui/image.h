#pragma once
/* RGBA images (0xAARRGGBB, straight alpha) decoded by the codec modules
 * from PNG files, SVG icons and the other formats of /lib/codecs. */
#include <stdint.h>
#include <stddef.h>

struct image {
    int w, h;                   /* device pixels */
    uint32_t *pixels;
    int scale;                  /* device pixels per logical pixel: 1 for PNG files */
};

/* Size in logical pixels (the painter draws images at that size). */
static inline int image_lw(const struct image *i) { return i->scale > 1 ? i->w / i->scale : i->w; }
static inline int image_lh(const struct image *i) { return i->scale > 1 ? i->h / i->scale : i->h; }

/* Decode a file, or data in memory, in any format that a codec module
 * decodes (<codec/codec.h>). The codec is selected by the content and
 * then by the extension. On failure the functions return NULL and set
 * errno, to ENOTSUP when no module decodes the format. */
struct image *image_load(const char *path);
struct image *image_decode(const uint8_t *data, size_t len);
/* Render an SVG icon with the svg codec at px by px pixels. color
 * (0x00rrggbb) fills paths that have no fill of their own. On failure
 * the functions return NULL and set errno. */
struct image *image_load_svg(const char *path, int px, uint32_t color);
struct image *image_render_svg(const char *text, size_t len, int px, uint32_t color);
void image_free(struct image *img);
/* A transparent image of w by h pixels (scale 1); NULL with errno. */
struct image *image_create(int w, int h);
/* A copy resampled to w by h pixels: area averages for reductions, the
 * nearest pixel for enlargements. NULL with errno. */
struct image *image_scale(const struct image *src, int w, int h);
/* PNG encoding (the png codec): RGB when every pixel is opaque, else RGBA.
 * image_encode_png stores a malloc'ed file in *data and returns its
 * length; both return a negative errno on failure. */
long image_encode_png(const struct image *img, uint8_t **data);
int image_save_png(const struct image *img, const char *path);
/* Inflate a zlib stream (RFC 1950) into dst. Returns the output length
 * or a negative errno value. This function calls codec_inflate. */
long zlib_inflate(uint8_t *dst, size_t cap, const uint8_t *src, size_t len);
