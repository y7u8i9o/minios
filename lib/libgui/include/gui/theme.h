#pragma once
/* Named colours and metrics shared by every widget of a process. */
#include <gui/gfx.h>

enum theme_color {
    TC_WINDOW, TC_TEXT, TC_TEXT_DISABLED, TC_FIELD, TC_SELECTION, TC_SELECTION_TEXT,
    TC_ACCENT, TC_BORDER, TC_HIGHLIGHT, TC_BUTTON, TC_BUTTON_PRESSED, TC_TRACK, TC_THUMB,
    TC_BUTTON_HOVER,
    TC_BUTTON_CHECKED,          /* a button that is on, such as a pressed toggle */
    /* The header bar of the client side decorations, for active windows
     * and for windows in the background (backdrop). */
    TC_HEADER, TC_HEADER_BACKDROP, TC_HEADER_LINE, TC_HEADER_LINE_BACKDROP, TC_TITLE, TC_TITLE_BACKDROP,
    TC_HEADER_BUTTON, TC_HEADER_BUTTON_HOVER, TC_HEADER_BUTTON_BACKDROP,
    TC_COUNT,
};

enum theme_metric {
    TM_PADDING, TM_SPACING, TM_BORDER, TM_RADIUS, TM_SCROLLBAR, TM_FONT_PX, TM_CONTROL_H,
    TM_ICON,                    /* the size of small icons */
    TM_ROW_PAD,                 /* the height of a row of lists, trees and tables above the font height */
    TM_INDENT,                  /* the indentation of a tree level */
    TM_COUNT,
};

struct theme {
    uint32_t color[TC_COUNT];
    int metric[TM_COUNT];       /* unscaled, in pixels at scale 100 */
    int scale;                  /* percent */
    const struct font *font;    /* set by theme_apply */
    char font_path[128];        /* outline font file, empty for the builtin font */
    char fallback_path[128];    /* optional Unicode fallback outline font */
    struct font *owned_font;
    struct font *owned_fallback;
};

/* The compiled in defaults: DejaVu Sans at 14 pixels when the file
 * exists, the builtin 8x16 font otherwise. */
void theme_init_default(struct theme *t);
/* Apply ui_font, ui_font_px and ui_scale from the configuration file, if present. */
void theme_read_conf(struct theme *t);
/* Load the font for the theme's scale; frees a previously loaded one. */
void theme_apply(struct theme *t);
void theme_release(struct theme *t);
/* A metric scaled by the theme scale. */
int theme_px(const struct theme *t, enum theme_metric m);
/* A size of px pixels at scale 100 scaled by the theme scale, for the
 * sizes that have no metric of their own. */
int theme_scale_px(const struct theme *t, int px);
