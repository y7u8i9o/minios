/* The icons of the panel: the SVG files of /usr/share/icons rendered at
 * the output scale in one colour, for the dark bar and for the light
 * menus. The icon cache of libgui (icon_lookup) contains each name once
 * per size, scale and colour. */
#include "panel.h"
#include <gui/image.h>
#include <gui/launcher.h>
#include <gui/widget.h>

const struct image *panel_icon(const char *name, uint32_t color)
{
    return icon_lookup(name, LAUNCHER_ICON, output_scale > 0 ? output_scale : 1, color);
}

const struct image *panel_app_icon(const char *command, uint32_t color)
{
    char name[LAUNCHER_COMMAND + 8];
    const struct image *img = panel_icon(launcher_icon_name(command, name, sizeof name), color);
    return img ? img : panel_icon("app-default", color);
}
