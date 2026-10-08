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
/* Intersect the clip with a local rectangle without moving the origin.
 * painter_pop restores the clip. */
void painter_push_clip(struct painter *p, int x, int y, int w, int h);
void painter_fill(struct painter *p, int x, int y, int w, int h, uint32_t color);
void painter_frame(struct painter *p, int x, int y, int w, int h, uint32_t color);
void painter_line(struct painter *p, int x0, int y0, int x1, int y1, uint32_t color);
/* The colour that paints nothing, for a fill or a border. */
#define PAINTER_NONE 0xffffffffu
/* A rectangle with corners of radius r logical pixels, antialiased from
 * the coverage tables of gui/pixel.h. border is one logical pixel wide
 * and lies inside the rectangle. fill or border may be PAINTER_NONE. */
void painter_round_rect(struct painter *p, int x, int y, int w, int h, int r, uint32_t fill, uint32_t border);
/* painter_round_rect with the theme radius. */
void painter_rounded(struct painter *p, int x, int y, int w, int h, uint32_t fill, uint32_t border);
/* An antialiased disc that fills the square of size by size at (x, y). */
void painter_disc(struct painter *p, int x, int y, int size, uint32_t color);
/* An antialiased ring in the square of size by size at (x, y): the disc
 * without the disc that lies width pixels inside it. */
void painter_ring(struct painter *p, int x, int y, int size, int width, uint32_t color);
/* An antialiased polyline through the n points xy[2 * i], xy[2 * i + 1]
 * in logical pixels, width logical pixels wide. (0, 0) is the top left
 * corner of the local area, so a line along pixel centres has
 * coordinates of the form k + 0.5. */
void painter_stroke(struct painter *p, const float *xy, int n, float width, uint32_t color);
/* A check mark in the square of size by size at (x, y). */
void painter_check(struct painter *p, int x, int y, int size, uint32_t color);
/* A chevron in the square of size by size at (x, y), pointing in the
 * direction dir. */
enum painter_dir { PAINTER_DOWN, PAINTER_UP, PAINTER_LEFT, PAINTER_RIGHT };
void painter_chevron(struct painter *p, int x, int y, int size, enum painter_dir dir, uint32_t color);
/* A fill of color with the opacity alpha (0 to 255) over the pixels. */
void painter_fill_alpha(struct painter *p, int x, int y, int w, int h, uint32_t color, int alpha);
void painter_text(struct painter *p, int x, int y, const char *text, uint32_t color);
/* Text with an explicit font and background (0xffffffff: transparent). */
void painter_text_font(struct painter *p, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg);
/* Text shaped by gfx_text_shape at the painter's scale, at logical (x, y). */
void painter_text_shaped(struct painter *p, int x, int y, const struct gfx_shaped *t, uint32_t fg);
int painter_text_width(const struct painter *p, const char *text, int n);
/* The width of the first n bytes of text in the font f at the scale of
 * the painter, in logical pixels rounded up. A NULL font is the font of
 * the theme. */
int painter_text_width_font(const struct painter *p, const struct font *f, const char *text, int n);
int painter_text_height(const struct painter *p);
/* Break text into lines no wider than w logical pixels. Lines break at
 * spaces and at newlines, and a space at a break is dropped. A word wider
 * than w is broken between characters. The function stores the byte offset
 * and length of the first max lines in start and len. It returns the
 * number of lines that the whole text needs, which may exceed max. */
int painter_wrap(const struct painter *p, const char *text, int w, int *start, int *len, int max);
/* Character index nearest to a local x offset within text. */
int painter_text_index(const struct painter *p, const char *text, int n, int px);
/* The same for the font f. A NULL font is the font of the theme. */
int painter_text_index_font(const struct painter *p, const struct font *f, const char *text, int n, int px);
/* A caption with a mnemonic. The marker '&' precedes the mnemonic
 * character. painter_mnemonic_strip copies text without the marker into
 * buf and returns the byte offset of the mnemonic character in buf, or -1
 * without a mnemonic. painter_mnemonic_text draws the caption without the
 * marker and underlines the mnemonic character. */
int painter_mnemonic_strip(const char *text, char *buf, int size);
void painter_mnemonic_text(struct painter *p, int x, int y, const char *text, uint32_t color);
/* A rounded ring in the accent colour around a focused control. */
void painter_focus_ring(struct painter *p, int x, int y, int w, int h);

/* The controls of libgui and of the panel. The colours come from the
 * theme of the painter. state combines the following bits. */
enum {
    PAINTER_HOVER = 1,          /* the pointer is over the control */
    PAINTER_PRESSED = 2,        /* pressed, or the popup of the control is open */
    PAINTER_CHECKED = 4,        /* on, such as an active task or a checked toggle */
    PAINTER_DISABLED = 8,
    PAINTER_FLAT = 16,          /* no background in the idle state */
    PAINTER_FOCUSED = 32,
};
/* The rounded background of a button: TC_TRACK when disabled, else
 * TC_BUTTON_PRESSED, TC_BUTTON_CHECKED, TC_BUTTON_HOVER or TC_BUTTON in
 * this order. A flat button in the idle state paints nothing. */
void painter_button(struct painter *p, int x, int y, int w, int h, int state);
/* The box of a text field: TC_FIELD, TC_BUTTON_HOVER with the pointer
 * over the box, TC_TRACK when disabled. The border is TC_ACCENT with the
 * focus, else TC_BORDER. */
void painter_field(struct painter *p, int x, int y, int w, int h, int state);
/* A slider: a track of w by h in TC_TRACK, filled with TC_ACCENT up to
 * pos, and a knob of knob_w by knob_h centred on pos and inside the
 * track. The knob is TC_THUMB, or TC_ACCENT with PAINTER_HOVER or
 * PAINTER_PRESSED, with a border of TC_BORDER. */
void painter_slider(struct painter *p, int x, int y, int w, int h, int pos, int knob_w, int knob_h, int state);
/* A meter: a track of w by h in TC_TRACK and a bar of len pixels from the
 * offset from in color. */
void painter_meter(struct painter *p, int x, int y, int w, int h, int from, int len, uint32_t color);
/* The background of a menu row: TC_HIGHLIGHT with PAINTER_HOVER or
 * PAINTER_CHECKED, else nothing. */
void painter_menu_row(struct painter *p, int x, int y, int w, int h, int state);
/* A card: TC_WINDOW with a border of TC_BORDER and corners of radius r. */
void painter_card(struct painter *p, int x, int y, int w, int h, int r);
/* A switch of PAINTER_SWITCH_W by PAINTER_SWITCH_H at (x, y): a pill in
 * TC_ACCENT when on, else TC_TRACK, with a knob of TC_FIELD on the side
 * of the state. */
#define PAINTER_SWITCH_W 36
#define PAINTER_SWITCH_H 18
void painter_switch(struct painter *p, int x, int y, int on, int state);
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
