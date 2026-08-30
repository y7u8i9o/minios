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
 * (.mfnt, written by tools/genfont/genfont.py) hold a 16 byte header
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
};

const struct font *gfx_font_builtin(void);      /* the 8x16 font */
struct font *gfx_font_load(const char *path);   /* .mfnt; NULL with errno on failure */
/* TrueType or OpenType file rendered at px pixels per em. */
struct font *gfx_font_open_ttf(const char *path, int px);
void gfx_font_free(struct font *f);
/* Blend an 8 bit coverage bitmap of color fg at (x, y). */
void gfx_blend_mask(struct surface *s, int x, int y, const uint8_t *mask, int w, int h, uint32_t fg);
void gfx_text_font(struct surface *s, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg);
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
/* Copy src into dst at (dx, dy), restricted to the clip rectangle of dst. */
void gfx_blit(struct surface *dst, int dx, int dy, const struct surface *src, const struct rect *clip);
/* Copy a rectangle of src to the same place in dst. */
void gfx_copy_rect(struct surface *dst, const struct surface *src, const struct rect *r);
/* Rectangle helpers. */
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
static inline int rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }
static inline int rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
