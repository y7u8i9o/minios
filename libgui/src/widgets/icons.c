/* Icon cache: /usr/share/icons/<name>.svg rendered at the output's
 * scale (Font Awesome icons in the text colour, folders in amber, the
 * quit mark in red), else <name>.png decoded once per process. */
#include <gui/widget.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ICON_MAX 96
#define ICON_PX 16

static struct { char name[32]; int px; struct image *img; } cache[ICON_MAX];
static int ncache;

static int device_scale(void)
{
    struct gui_output_info info;
    if (gui_output_count() > 0 && gui_get_output(0, &info) == 0 && info.scale > 0 && info.scale <= 4)
        return info.scale;
    return 1;
}

static uint32_t icon_color(const char *name)
{
    if (strcmp(name, "folder") == 0 || strcmp(name, "open") == 0)
        return 0x00d9a520;
    if (strcmp(name, "quit") == 0)
        return 0x00c04040;
    return 0x002a2a2a;
}

static struct image *load_svg(const char *name, int px)
{
    char path[128];
    snprintf(path, sizeof path, "/usr/share/icons/%s.svg", name);
    int scale = device_scale();
    struct image *img = image_load_svg(path, px * scale, icon_color(name));
    if (img)
        img->scale = scale;
    return img;
}

static const struct image *lookup(const char *name, int px, int png_fallback)
{
    for (int i = 0; i < ncache; i++)
        if (cache[i].px == px && strcmp(cache[i].name, name) == 0)
            return cache[i].img;
    if (ncache == ICON_MAX)
        return NULL;
    struct image *img = load_svg(name, px);
    if (!img && png_fallback) {
        char path[128];
        snprintf(path, sizeof path, "/usr/share/icons/%s.png", name);
        img = image_load(path);
    }
    strlcpy(cache[ncache].name, name, sizeof cache[ncache].name);
    cache[ncache].px = px;
    cache[ncache].img = img;                /* NULL is cached too */
    ncache++;
    return img;
}

const struct image *icon_get(const char *name) { return lookup(name, ICON_PX, 1); }

const struct image *icon_get_size(const char *name, int px) { return lookup(name, px, 0); }
