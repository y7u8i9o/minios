#pragma once
/* Software drawing on 32 bit RGB surfaces. Coordinates are clipped to the
 * surface. Colors are 0x00RRGGBB. */
#include <stdint.h>
#include <stddef.h>

#define GFX_FONT_W 8
#define GFX_FONT_H 16

struct surface {
    uint32_t *pixels;
    int width, height;
    int stride;             /* pixels per row */
};

struct rect {
    int x, y, w, h;
};

extern const uint8_t gfx_font8x16[256][16];

/* Bitmap fonts: 256 glyphs, 1 bit per pixel, variable advance. Files
 * (.mfnt, written by tools/genfont/genfont.py) contain a 16 byte header
 * ("MFNT", uint16 height, ascent, max width, glyph count, uint32
 * reserved), 256 advances, 256 widths, then per glyph height rows of
 * 32 bits with bit 31 leftmost. */
struct ofont;

struct font {
    int height, ascent;
    uint8_t advance[256], width[256];
    const uint32_t *bits;       /* [256][height] */
    uint32_t *owned;            /* storage released by gfx_font_free */
    /* Outline fonts (libfont): when set, text is shaped with kerning
     * and rasterized with antialiasing at px pixels per em. */
    struct ofont *outline;
    int px;
    const struct font *fallback; /* non-owning, used when a glyph is absent */
};

const struct font *gfx_font_builtin(void);      /* the 8x16 font */
struct font *gfx_font_load(const char *path);   /* .mfnt; NULL with errno on failure */
/* TrueType or OpenType file rendered at px pixels per em. */
struct font *gfx_font_open_ttf(const char *path, int px);
void gfx_font_set_fallback(struct font *font, const struct font *fallback);
/* The fallback font for Chinese and Japanese text at px pixels, or NULL
 * (/usr/share/fonts/DroidSansFallbackFull.ttf).  The text functions take CJK
 * characters that the font and its fallbacks lack from it. */
const struct font *gfx_font_cjk(int px);
void gfx_font_free(struct font *f);
/* Blend an 8 bit coverage bitmap of color fg at (x, y). */
void gfx_blend_mask(struct surface *s, int x, int y, const uint8_t *mask, int w, int h, uint32_t fg);
void gfx_text_font(struct surface *s, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg);
/* The same at an integer device scale: outline fonts are rasterized at
 * px * scale, bitmap fonts are drawn with scale by scale blocks; x, y and
 * the returned widths are device pixels. Heights are height * scale. */
void gfx_text_font_scaled(struct surface *s, const struct font *f, int x, int y, const char *text, uint32_t fg,
                          uint32_t bg, int scale);
int gfx_text_width_font_scaled(const struct font *f, const char *text, int n, int scale);
int gfx_text_index_font_scaled(const struct font *f, const char *text, int n, int px, int scale);
/* A line of text shaped once at an integer device scale, for repeated
 * drawing without shaping again (the titles of X12). The width is in
 * device pixels; gfx_shaped_draw draws at device pixels as
 * gfx_text_font_scaled does. NULL without memory. */
struct gfx_shaped;
struct gfx_shaped *gfx_text_shape(const struct font *f, const char *text, int scale);
int gfx_shaped_width(const struct gfx_shaped *t);
void gfx_shaped_draw(struct surface *s, const struct gfx_shaped *t, int x, int y, uint32_t fg);
void gfx_shaped_free(struct gfx_shaped *t);
/* Width of the first n characters (n < 0: the whole string). */
int gfx_text_width_font(const struct font *f, const char *text, int n);
/* Index of the character boundary nearest to pixel offset px. */
int gfx_text_index_font(const struct font *f, const char *text, int n, int px);

static inline uint32_t gfx_rgb(int r, int g, int b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void gfx_fill(struct surface *s, uint32_t color);
void gfx_fill_rect(struct surface *s, int x, int y, int w, int h, uint32_t color);
void gfx_rect(struct surface *s, int x, int y, int w, int h, uint32_t color);
void gfx_hline(struct surface *s, int x, int y, int w, uint32_t color);
void gfx_vline(struct surface *s, int x, int y, int h, uint32_t color);
void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, uint32_t color);
/* Draw text with the 8x16 font; bg 0xffffffff means transparent. */
void gfx_text(struct surface *s, int x, int y, const char *text, uint32_t fg, uint32_t bg);
int gfx_text_width(const char *text);
/* An antialiased disc of the colour over the square of size device pixels
 * at (x, y): the disc of diameter size - inset centred in it, clipped to
 * clip (device pixels) when not NULL and to s (pixel_disc_table). */
void gfx_disc(struct surface *s, int x, int y, int size, int inset, uint32_t color, const struct rect *clip);
/* Copy src into dst at (dx, dy), restricted to the clip rectangle of dst. */
void gfx_blit(struct surface *dst, int dx, int dy, const struct surface *src, const struct rect *clip);
/* Copy a rectangle of src to the same place in dst. */
void gfx_copy_rect(struct surface *dst, const struct surface *src, const struct rect *r);
/* Move the pixels inside r by (dx, dy) within s. Pixels that the move
 * takes out of r are dropped, and the area that it exposes retains its
 * old pixels. r must lie inside s. */
void gfx_move_rect(struct surface *s, struct rect r, int dx, int dy);
/* Rectangle helpers. */
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
static inline int rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }
static inline int rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
/* r with every coordinate multiplied by s, for logical to device pixels. */
static inline struct rect rect_scale(struct rect r, int s)
{
    return (struct rect){ r.x * s, r.y * s, r.w * s, r.h * s };
}
/* a minus b as up to four disjoint rectangles in out. Returns their
 * number: 1 with a itself when the two do not intersect, 0 when b covers a. */
int rect_subtract(struct rect a, struct rect b, struct rect out[4]);

/* A set of up to RECT_SET_MAX disjoint rectangles, for damage
 * (docs/design/graphics-performance.md). rect_set_add merges a new
 * rectangle with one of the set when their bounding box adds at most a
 * quarter of their area, and otherwise adds the parts of it that the set
 * does not cover yet. A set that would exceed its size becomes the
 * bounding box of everything. The set may therefore cover more than the
 * union of the added rectangles, but never less. */
#define RECT_SET_MAX 32
struct rect_set {
    int n;
    struct rect r[RECT_SET_MAX];
};
void rect_set_clear(struct rect_set *s);
void rect_set_add(struct rect_set *s, struct rect r);
struct rect rect_set_bounds(const struct rect_set *s);

/* The edges of a window resize.  The values are those of the resize
 * request of the window protocol. */
enum { GUI_EDGE_TOP = 1, GUI_EDGE_BOTTOM = 2, GUI_EDGE_LEFT = 4, GUI_EDGE_RIGHT = 8 };

/* The resize zones around a window frame, for the decorations of the
 * compositor and of the client library (docs/design/compositor.md):
 *   margin      the width of the edge zones outside the frame
 *   inner       the width of the edge zones inside the frame, a border
 *   corner      the length of a corner zone along each edge, from the corner
 *   reach       the distance of the corner squares outside the frame
 *   inset_top   the distance of the top corner squares inside the frame,
 *               the radius of rounded top corners
 *   inset_bottom  the same for the bottom corners */
struct gui_resize_zones {
    int margin, inner, corner, reach, inset_top, inset_bottom;
};

/* gui_resize_edges returns the edges that a press at (x, y) resizes for the
 * frame f, or 0 outside the resize zones. */
int gui_resize_edges(struct rect f, const struct gui_resize_zones *z, int x, int y);
/* gui_resize_region stores the area of the frame with its resize zones in
 * out: the frame grown by margin and the four corner squares.  The result
 * is the number of rectangles, 5. */
int gui_resize_region(struct rect f, const struct gui_resize_zones *z, struct rect out[5]);

/* Colours as 0x00RRGGBB in hue, saturation and value: the hue in degrees
 * from 0 to 359, saturation and value from 0 to 255. A conversion to HSV
 * and back changes each channel by at most 3. */
void gfx_rgb_to_hsv(uint32_t rgb, int *h, int *s, int *v);
uint32_t gfx_hsv_to_rgb(int h, int s, int v);
/* Parses "#rrggbb", "0xrrggbb" or "rrggbb" into rgb. Returns 0, or -1
 * for another text. */
int gfx_color_parse(const char *text, uint32_t *rgb);
