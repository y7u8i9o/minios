#pragma once
/* Row operations on 32 bit pixels for libgui and X12
 * (docs/design/graphics-performance.md). A pixel is 0xAARRGGBB. The alpha
 * of a source pixel is straight, not premultiplied. Every function works
 * on n pixels of one row. The blending functions use NEON on aarch64. On
 * x86_64 they use SSE2 or a 64 bit scalar form, whichever the first call
 * measures as faster (QEMU emulates SSE2 slowly under TCG). A scalar
 * reference of each function in src/pixel_impl.h gives the same result,
 * and the tests compare every form with it.
 *
 * A blend divides by 255 with rounding. Alpha 0 leaves the destination
 * and alpha 255 gives the source colour exactly. A blended pixel retains
 * the alpha byte of the destination. */
#include <stdint.h>

/* t / 255 rounded to the nearest integer, for t up to 255 * 255. */
static inline uint32_t pixel_div255(uint32_t t)
{
    t += 128;
    return (t + (t >> 8)) >> 8;
}

/* The colour c over the pixel d with coverage a (0 to 255). */
static inline uint32_t pixel_blend(uint32_t d, uint32_t c, uint32_t a)
{
    uint32_t ia = 255 - a;
    uint32_t r = pixel_div255((d >> 16 & 0xff) * ia + (c >> 16 & 0xff) * a);
    uint32_t g = pixel_div255((d >> 8 & 0xff) * ia + (c >> 8 & 0xff) * a);
    uint32_t b = pixel_div255((d & 0xff) * ia + (c & 0xff) * a);
    return (d & 0xff000000u) | r << 16 | g << 8 | b;
}

/* The pixel d with each colour channel multiplied by keep / 255. */
static inline uint32_t pixel_shade(uint32_t d, uint32_t keep)
{
    uint32_t r = pixel_div255((d >> 16 & 0xff) * keep);
    uint32_t g = pixel_div255((d >> 8 & 0xff) * keep);
    uint32_t b = pixel_div255((d & 0xff) * keep);
    return (d & 0xff000000u) | r << 16 | g << 8 | b;
}

/* to[i] = color. */
void pixel_fill(uint32_t *to, int n, uint32_t color);
/* to[i] = the colour of from[i] with the alpha byte 0, for an opaque copy
 * of an ARGB or XRGB source to the screen. */
void pixel_copy_opaque(uint32_t *to, const uint32_t *from, int n);
/* to[i] = from[i] over to[i] with the alpha of from[i]. */
void pixel_over(uint32_t *to, const uint32_t *from, int n);
/* to[i] = color over to[i] with the coverage mask[i]. */
void pixel_mask(uint32_t *to, const uint8_t *mask, int n, uint32_t color);
/* to[i] = to[i] with each colour channel multiplied by keep / 255. */
void pixel_darken(uint32_t *to, int n, uint32_t keep);
/* The forms that pixel_over, pixel_mask and pixel_darken use, for
 * diagnostics, for example "over word, mask word, darken simd". */
const char *pixel_forms(void);

/* The nearest neighbour positions floor((start + i) * num / den) for
 * i = 0, 1, 2 and so on, without a division per position. num and den
 * are positive, and start may be negative. */
struct pixel_walk {
    long pos, rem;                      /* the next position, the remainder of its division */
    long inc, rinc, den;
};
void pixel_walk_init(struct pixel_walk *w, long start, long num, long den);
/* Advance the walk by one position. */
static inline void pixel_walk_next(struct pixel_walk *w)
{
    w->pos += w->inc;
    w->rem += w->rinc;
    if (w->rem >= w->den) {
        w->rem -= w->den;
        w->pos++;
    }
}
/* to[i] = from[p * step] for the next n positions p of the walk. A step
 * of -1 reads a row backwards, a step of a stride reads a column. */
void pixel_sample(uint32_t *to, const uint32_t *from, long step, int n, struct pixel_walk *w);
/* The same for a coverage mask. */
void pixel_sample_mask(uint8_t *to, const uint8_t *from, int n, struct pixel_walk *w);
/* from[p * step] over to[i] for the next n positions p of the walk:
 * pixel_sample followed by pixel_over. */
void pixel_sample_over(uint32_t *to, const uint32_t *from, long step, int n, struct pixel_walk *w);
/* color over to[i] with the coverage mask[p] for the next n positions p
 * of the walk: pixel_sample_mask followed by pixel_mask. */
void pixel_mask_sample(uint32_t *to, const uint8_t *mask, int n, uint32_t color, struct pixel_walk *w);

/* Antialiased shapes from coverage tables (G7 of
 * docs/plan/compositor-performance.md). A coverage is 0 to 255; 255 means
 * that the shape covers the pixel completely, so a pixel partly covered
 * has at most 254. The tables are computed once per size with the float
 * formula c = r + 0.5 - distance of the pixel centre from the centre of
 * the arc, and cached for the life of the process. The cache belongs to
 * the thread that draws, which is the only one in a libgui program and in
 * X12. */

/* The coverage of the r by r pixels of a corner square by a quarter circle
 * of radius r: entry j * r + i for the pixel i columns and j rows away from
 * the outer corner. NULL for r < 1 or without memory. */
const uint8_t *pixel_corner_table(int r);
/* The coverage of the size by size pixels of a square by the disc of
 * diameter size - inset centred in it: entry j * size + i. NULL for a size
 * below 1 or without memory. */
const uint8_t *pixel_disc_table(int size, int inset);

#define PIXEL_CORNER_TL 1
#define PIXEL_CORNER_TR 2
#define PIXEL_CORNER_BL 4
#define PIXEL_CORNER_BR 8
/* The coverage of the pixel (x, y) by the rectangle with the left edge x0,
 * the top edge y0 and the size w by h, whose corners in the mask corners
 * are rounded with radius r; table is pixel_corner_table(r). */
static inline uint32_t pixel_round_rect_coverage(const uint8_t *table, int r, int x0, int y0, int w, int h,
                                                 int corners, int x, int y)
{
    if (x < x0 || y < y0 || x >= x0 + w || y >= y0 + h)
        return 0;
    if (r <= 0 || !table)
        return 255;
    int i = x - x0, j = y - y0, left = 1;
    if (i >= r) {
        i = x0 + w - 1 - x;
        left = 0;
        if (i >= r)
            return 255;
    }
    int top = 1;
    if (j >= r) {
        j = y0 + h - 1 - y;
        top = 0;
        if (j >= r)
            return 255;
    }
    int corner = top ? (left ? PIXEL_CORNER_TL : PIXEL_CORNER_TR) : (left ? PIXEL_CORNER_BL : PIXEL_CORNER_BR);
    return corners & corner ? table[j * r + i] : 255;
}

/* A framebuffer format that is not 0x00RRGGBB in 32 bits: 3 or 4 bytes
 * per pixel, and the size and position of each colour channel. */
struct pixel_format {
    int bytes;
    int red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};
/* Convert n pixels into the format at to. */
void pixel_pack(uint8_t *to, const uint32_t *from, int n, const struct pixel_format *f);
