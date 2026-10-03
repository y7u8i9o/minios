/* Theme defaults, scaling and the interface font. */
#include <minios/conf.h>
#include <gui/theme.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void theme_init_default(struct theme *t)
{
    memset(t, 0, sizeof *t);
    /* Light neutral greys with faint borders, matching the window
     * chrome drawn by csd.c (header 0xebebeb, outline 20 percent black). */
    t->color[TC_WINDOW] = 0x00ebebeb;       /* the header bar's grey */
    t->color[TC_TEXT] = 0x002a2a2a;
    t->color[TC_TEXT_DISABLED] = 0x009a9a9a;
    t->color[TC_FIELD] = 0x00ffffff;
    t->color[TC_SELECTION] = 0x003c78c8;
    t->color[TC_SELECTION_TEXT] = 0x00ffffff;
    t->color[TC_ACCENT] = 0x003c78c8;
    t->color[TC_BORDER] = 0x00b0b0b0;
    t->color[TC_HIGHLIGHT] = 0x00d6e2f4;
    t->color[TC_BUTTON] = 0x00dcdcdc;
    t->color[TC_BUTTON_HOVER] = 0x00d0d0d0;
    t->color[TC_BUTTON_PRESSED] = 0x00bcbcbc;
    t->color[TC_TRACK] = 0x00dedede;
    t->color[TC_THUMB] = 0x00a8a8a8;
    t->metric[TM_PADDING] = 6;
    t->metric[TM_SPACING] = 6;
    t->metric[TM_BORDER] = 1;
    t->metric[TM_RADIUS] = 5;
    t->metric[TM_SCROLLBAR] = 14;
    t->metric[TM_FONT_PX] = 14;
    t->metric[TM_CONTROL_H] = 26;
    t->scale = 100;
    strlcpy(t->font_path, "/usr/share/fonts/DejaVuSans.ttf", sizeof t->font_path);
    strlcpy(t->fallback_path, "/usr/share/fonts/DejaVuSansMono.ttf", sizeof t->fallback_path);
    t->font = gfx_font_builtin();
    theme_read_conf(t);
}

/* The interface font, its size and the scale come from the configuration
 * file (conf_read_path)
 * (ui_font, ui_font_px, ui_scale), written by the settings program. A
 * missing file keeps the defaults. */
static const struct { const char *name; const char *path; } ui_fonts[] = {
    { "DejaVu Sans", "/usr/share/fonts/DejaVuSans.ttf" },
    { "Noto Sans", "/usr/share/fonts/NotoSans-Regular.ttf" },
    { "Latin Modern Roman", "/usr/share/fonts/lmroman10-regular.otf" },
    { "Builtin bitmap font", "" },
};

void theme_read_conf(struct theme *t)
{
    char path[256];
    FILE *f = fopen(conf_read_path(path, sizeof path), "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = '\0';
        const char *v = eq + 1;
        if (strcmp(line, "ui_font") == 0) {
            for (size_t i = 0; i < sizeof ui_fonts / sizeof ui_fonts[0]; i++)
                if (strcmp(ui_fonts[i].name, v) == 0)
                    strlcpy(t->font_path, ui_fonts[i].path, sizeof t->font_path);
        } else if (strcmp(line, "ui_font_px") == 0) {
            int px = atoi(v);
            if (px >= 8 && px <= 32)
                t->metric[TM_FONT_PX] = px;
        } else if (strcmp(line, "ui_scale") == 0) {
            int s = atoi(v);
            if (s >= 50 && s <= 300)
                t->scale = s;
        }
    }
    fclose(f);
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
