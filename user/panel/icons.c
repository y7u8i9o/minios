/* The icons of the panel: the SVG files of /usr/share/icons rendered at
 * the output scale in one colour, for the dark bar and for the light
 * menus. The cache contains each name once per scale and colour. A
 * missing icon is cached as NULL. */
#include "panel.h"
#include <stdio.h>
#include <string.h>
#include <gui/image.h>
#include <gui/launcher.h>

#define MAX_ICONS 96

static struct {
    char name[40];
    int scale;
    uint32_t color;
    struct image *img;
} icons[MAX_ICONS];
static int nicons;

const struct image *panel_icon(const char *name, uint32_t color)
{
    int scale = output_scale > 0 ? output_scale : 1;
    for (int i = 0; i < nicons; i++)
        if (icons[i].scale == scale && icons[i].color == color && strcmp(icons[i].name, name) == 0)
            return icons[i].img;
    if (nicons == MAX_ICONS)
        return NULL;
    char path[96];
    snprintf(path, sizeof path, LAUNCHER_ICON_DIR "/%s.svg", name);
    struct image *img = image_load_svg(path, LAUNCHER_ICON * scale, color);
    if (img)
        img->scale = scale;
    strlcpy(icons[nicons].name, name, sizeof icons[nicons].name);
    icons[nicons].scale = scale;
    icons[nicons].color = color;
    icons[nicons].img = img;
    nicons++;
    return img;
}

const struct image *panel_app_icon(const char *command, uint32_t color)
{
    char name[LAUNCHER_COMMAND + 8];
    const struct image *img = panel_icon(launcher_icon_name(command, name, sizeof name), color);
    return img ? img : panel_icon("app-default", color);
}
