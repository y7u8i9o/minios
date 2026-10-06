#pragma once
/* The scalar references of the row operations of gui/pixel.h. pixel.c
 * uses them for the pixels after the last whole vector. The tests
 * (tests/test_pixel.c on the host, pixeltest in the guest) compare the
 * vector functions with them. */
#include <gui/pixel.h>

/* The vector and word forms of the blending functions (pixel.c). */
void pixel_over_simd(uint32_t *to, const uint32_t *from, int n);
void pixel_over_word(uint32_t *to, const uint32_t *from, int n);
void pixel_mask_simd(uint32_t *to, const uint8_t *mask, int n, uint32_t color);
void pixel_mask_word(uint32_t *to, const uint8_t *mask, int n, uint32_t color);
void pixel_darken_simd(uint32_t *to, int n, uint32_t keep);
void pixel_darken_word(uint32_t *to, int n, uint32_t keep);

static inline void pixel_fill_ref(uint32_t *to, int n, uint32_t color)
{
    for (int i = 0; i < n; i++)
        to[i] = color;
}

static inline void pixel_copy_opaque_ref(uint32_t *to, const uint32_t *from, int n)
{
    for (int i = 0; i < n; i++)
        to[i] = from[i] & 0x00ffffffu;
}

static inline void pixel_over_ref(uint32_t *to, const uint32_t *from, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t c = from[i], a = c >> 24;
        if (a)
            to[i] = pixel_blend(to[i], c, a);
    }
}

static inline void pixel_mask_ref(uint32_t *to, const uint8_t *mask, int n, uint32_t color)
{
    for (int i = 0; i < n; i++)
        if (mask[i])
            to[i] = pixel_blend(to[i], color, mask[i]);
}

static inline void pixel_darken_ref(uint32_t *to, int n, uint32_t keep)
{
    for (int i = 0; i < n; i++)
        to[i] = pixel_shade(to[i], keep);
}
