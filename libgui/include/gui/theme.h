#pragma once
/* Named colours and metrics shared by every widget of a process. */
#include <gui/gfx.h>

enum theme_color {
    TC_WINDOW, TC_TEXT, TC_TEXT_DISABLED, TC_FIELD, TC_SELECTION, TC_SELECTION_TEXT,
    TC_ACCENT, TC_BORDER, TC_HIGHLIGHT, TC_BUTTON, TC_BUTTON_PRESSED, TC_TRACK, TC_THUMB,
    TC_COUNT,
};

enum theme_metric {
    TM_PADDING, TM_SPACING, TM_BORDER, TM_RADIUS, TM_SCROLLBAR, TM_FONT_PX, TM_CONTROL_H,
    TM_COUNT,
};

struct theme {
    uint32_t color[TC_COUNT];
    int metric[TM_COUNT];       /* unscaled, in pixels at scale 100 */
    int scale;                  /* percent */
    const struct font *font;    /* set by theme_apply */
    char font_path[128];        /* outline font file, empty for the builtin font */
    struct font *owned_font;
};

/* The compiled in defaults: DejaVu Sans at 14 pixels when the file
 * exists, the builtin 8x16 font otherwise. */
void theme_init_default(struct theme *t);
/* Load the font for the theme's scale; frees a previously loaded one. */
void theme_apply(struct theme *t);
void theme_release(struct theme *t);
/* A metric scaled by the theme scale. */
int theme_px(const struct theme *t, enum theme_metric m);
