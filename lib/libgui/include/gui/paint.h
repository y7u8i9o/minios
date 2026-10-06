#pragma once
/* Painting bound to a surface with an origin and a clip stack, so
 * widgets draw in local coordinates and never outside their area. */
#include <gui/gfx.h>
#include <gui/theme.h>
#include <gui/image.h>

#define PAINTER_DEPTH 16

/* Local coordinates are logical pixels; the surface contains scale by scale
 * device pixels per logical pixel (1 unless the window is on a high
 * density output). Origin and clip are stored in device pixels. */
struct painter {
    struct surface *s;
    const struct theme *theme;
    int scale;                  /* device pixels per logical pixel */
    int ox, oy;                 /* origin of local coordinates in the surface */
    struct rect clip;           /* current clip in surface coordinates */
    struct rect stack[PAINTER_DEPTH];
    int ostack[PAINTER_DEPTH][2];
    int depth;
};

void painter_init(struct painter *p, struct surface *s, const struct theme *theme);
/* The same for a surface with scale device pixels per logical pixel. */
void painter_init_scaled(struct painter *p, struct surface *s, const struct theme *theme, int scale);
/* Enter a child area: translate the origin and intersect the clip. */
void painter_push(struct painter *p, int x, int y, int w, int h);
void painter_pop(struct painter *p);
void painter_fill(struct painter *p, int x, int y, int w, int h, uint32_t color);
void painter_frame(struct painter *p, int x, int y, int w, int h, uint32_t color);
void painter_line(struct painter *p, int x0, int y0, int x1, int y1, uint32_t color);
/* Filled rounded rectangle with the theme radius. */
void painter_rounded(struct painter *p, int x, int y, int w, int h, uint32_t fill, uint32_t border);
void painter_text(struct painter *p, int x, int y, const char *text, uint32_t color);
/* Text with an explicit font and background (0xffffffff: transparent). */
void painter_text_font(struct painter *p, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg);
/* Text shaped by gfx_text_shape at the painter's scale, at logical (x, y). */
void painter_text_shaped(struct painter *p, int x, int y, const struct gfx_shaped *t, uint32_t fg);
int painter_text_width(const struct painter *p, const char *text, int n);
int painter_text_height(const struct painter *p);
/* Break text into lines no wider than w logical pixels. Lines break at
 * spaces and at newlines, and a space at a break is dropped. A word wider
 * than w is broken between characters. The function stores the byte offset
 * and length of the first max lines in start and len. It returns the
 * number of lines that the whole text needs, which may exceed max. */
int painter_wrap(const struct painter *p, const char *text, int w, int *start, int *len, int max);
/* Character index nearest to a local x offset within text. */
int painter_text_index(const struct painter *p, const char *text, int n, int px);
void painter_focus_ring(struct painter *p, int x, int y, int w, int h);
/* The avatar of an account: a rounded square of size by size in a colour
 * that the account name selects, with the first letter of label, which is
 * the full name, in white. */
void painter_avatar(struct painter *p, int x, int y, int size, const char *name, const char *label);
/* Blit a 32 bit surface at (x, y). */
void painter_blit(struct painter *p, int x, int y, const struct surface *src);
/* Blend an 8 bit mask. */
void painter_mask(struct painter *p, int x, int y, const uint8_t *mask, int w, int h, uint32_t color);
/* Blend an RGBA image at (x, y). */
void painter_image(struct painter *p, int x, int y, const struct image *img);
/* Draw an image scaled to w by h logical pixels at (x, y). */
void painter_image_scaled(struct painter *p, int x, int y, int w, int h, const struct image *img);
/* Clip rectangle in local coordinates (for skipping invisible work). */
struct rect painter_clip_local(const struct painter *p);
