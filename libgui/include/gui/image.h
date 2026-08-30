#pragma once
/* RGBA images (0xAARRGGBB, straight alpha) decoded from PNG files. */
#include <stdint.h>
#include <stddef.h>

struct image {
    int w, h;
    uint32_t *pixels;
};

struct image *image_load(const char *path);                 /* NULL with errno */
struct image *image_decode(const uint8_t *data, size_t len); /* PNG in memory */
void image_free(struct image *img);
/* zlib stream (RFC 1950) to dst; returns the output length or a negative errno. */
long zlib_inflate(uint8_t *dst, size_t cap, const uint8_t *src, size_t len);
