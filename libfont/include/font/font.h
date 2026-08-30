#pragma once
/* libfont: TrueType and CFF flavoured OpenType fonts parsed from files,
 * outlines rasterized to 8 bit coverage bitmaps with fixed point
 * arithmetic, kerning from the kern and GPOS tables. Independent from
 * the window system; libgui wraps it behind struct ofont. */
#include <stdint.h>
#include <stddef.h>

/* Outline point in font units; on_curve is 0 for control points. Cubic
 * curves (CFF) are converted to quadratics by the parser, so every
 * off-curve point is a quadratic control point. */
struct font_point {
    int32_t x, y;
    uint8_t on_curve;
};

struct font_outline {
    struct font_point *points;
    int npoints;
    int *contour_end;           /* index of the last point of each contour */
    int ncontours;
    int32_t xmin, ymin, xmax, ymax;
};

/* A rasterized glyph: coverage 0..255, top-down rows. left and top are
 * the bearing from the pen position (y grows downwards); advance is in
 * 26.6 pixels. */
struct font_glyph {
    uint8_t *bitmap;
    int width, height;
    int left, top;
    int32_t advance;
};

struct ofont;

struct ofont *font_open(const char *path);
void font_close(struct ofont *f);
const char *font_name(const struct ofont *f);       /* file path */
int font_units_per_em(const struct ofont *f);
int font_glyph_count(const struct ofont *f);
int font_is_cff(const struct ofont *f);
/* Ascent (positive), descent (negative) and line gap in font units. */
void font_metrics(const struct ofont *f, int *ascent, int *descent, int *line_gap);
/* Glyph id of a code point, 0 when missing. */
int font_glyph_index(const struct ofont *f, uint32_t codepoint);
int font_advance(const struct ofont *f, int glyph);   /* font units */
int font_lsb(const struct ofont *f, int glyph);
/* Kerning adjustment for the pair in font units (negative moves closer). */
int font_kern(const struct ofont *f, int left, int right);
/* Outline of a glyph in font units; free with font_outline_free. Returns
 * 0, or a negative errno. Empty glyphs give zero contours. */
int font_outline(const struct ofont *f, int glyph, struct font_outline *out);
void font_outline_free(struct font_outline *o);

/* Rasterize a glyph at px pixels per em. The bitmap belongs to the
 * font's cache: valid until font_close or until the cache evicts it
 * (after FONT_CACHE_SIZE other lookups); copy it when keeping it. */
#define FONT_CACHE_SIZE 512
const struct font_glyph *font_render(struct ofont *f, int glyph, int px);
/* Scale font units to 26.6 pixels at px pixels per em. */
int32_t font_scale(const struct ofont *f, int units, int px);

/* Shaping: glyph ids and pen positions (26.6 pixels) for a string of
 * bytes interpreted as ISO 8859-1, with kerning applied. Returns the
 * number of glyphs written (at most max); *width receives the total
 * advance in 26.6 pixels. */
struct font_shaped {
    int glyph;
    int32_t x;                  /* pen x in 26.6 pixels */
};
int font_shape(const struct ofont *f, const char *text, int n, int px, struct font_shaped *out, int max, int32_t *width);
