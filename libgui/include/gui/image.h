#pragma once
/* RGBA images (0xAARRGGBB, straight alpha) decoded from PNG files or
 * rendered from SVG icons. */
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

struct image *image_load(const char *path);                 /* NULL with errno */
struct image *image_decode(const uint8_t *data, size_t len); /* PNG in memory */
/* An SVG icon (src/svg.c) rendered px by px pixels; color is the fill
 * of paths without one (0x00rrggbb). NULL with errno. */
struct image *image_load_svg(const char *path, int px, uint32_t color);
struct image *image_render_svg(const char *text, size_t len, int px, uint32_t color);
void image_free(struct image *img);
/* zlib stream (RFC 1950) to dst; returns the output length or a negative errno. */
long zlib_inflate(uint8_t *dst, size_t cap, const uint8_t *src, size_t len);
