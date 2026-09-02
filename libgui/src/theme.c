/* Theme defaults, scaling and the interface font. */
#include <gui/theme.h>
#include <string.h>
#include <stdlib.h>

void theme_init_default(struct theme *t)
{
    memset(t, 0, sizeof *t);
    t->color[TC_WINDOW] = 0x00e4e4e4;
    t->color[TC_TEXT] = 0x00101010;
    t->color[TC_TEXT_DISABLED] = 0x00808080;
    t->color[TC_FIELD] = 0x00ffffff;
    t->color[TC_SELECTION] = 0x003060b0;
    t->color[TC_SELECTION_TEXT] = 0x00ffffff;
    t->color[TC_ACCENT] = 0x002050a0;
    t->color[TC_BORDER] = 0x00505050;
    t->color[TC_HIGHLIGHT] = 0x00c8d8f0;
    t->color[TC_BUTTON] = 0x00f4f4f4;
    t->color[TC_BUTTON_PRESSED] = 0x00b0b0b0;
    t->color[TC_TRACK] = 0x00c8c8c8;
    t->color[TC_THUMB] = 0x00808080;
    t->metric[TM_PADDING] = 6;
    t->metric[TM_SPACING] = 6;
    t->metric[TM_BORDER] = 1;
    t->metric[TM_RADIUS] = 3;
    t->metric[TM_SCROLLBAR] = 14;
    t->metric[TM_FONT_PX] = 14;
    t->metric[TM_CONTROL_H] = 26;
    t->scale = 100;
    strlcpy(t->font_path, "/etc/fonts/DejaVuSans.ttf", sizeof t->font_path);
    strlcpy(t->fallback_path, "/etc/fonts/DejaVuSansMono.ttf", sizeof t->fallback_path);
    t->font = gfx_font_builtin();
}

int theme_px(const struct theme *t, enum theme_metric m)
{
    return (t->metric[m] * t->scale + 50) / 100;
}

void theme_apply(struct theme *t)
{
    struct font *f = NULL;
    struct font *fallback = NULL;
    if (t->font_path[0])
        f = gfx_font_open_ttf(t->font_path, theme_px(t, TM_FONT_PX));
    if (f && t->fallback_path[0] && strcmp(t->fallback_path, t->font_path) != 0)
        fallback = gfx_font_open_ttf(t->fallback_path, theme_px(t, TM_FONT_PX));
    if (t->owned_font)
        gfx_font_free(t->owned_font);
    if (t->owned_fallback)
        gfx_font_free(t->owned_fallback);
    t->owned_font = f;
    t->owned_fallback = fallback;
    if (f)
        gfx_font_set_fallback(f, fallback);
    t->font = f ? f : gfx_font_builtin();
}

void theme_release(struct theme *t)
{
    if (t->owned_font)
        gfx_font_free(t->owned_font);
    if (t->owned_fallback)
        gfx_font_free(t->owned_fallback);
    t->owned_font = NULL;
    t->owned_fallback = NULL;
    t->font = gfx_font_builtin();
}
