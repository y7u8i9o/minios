/* Icon cache: /usr/share/icons/<name>.svg rendered at a scale and in a
 * colour (Font Awesome icons in the text colour, folders in amber, the
 * quit mark in red), else <name>.png decoded once per process. The key of
 * an entry consists of name, size, scale and colour. */
#include <gui/widget.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ICON_PX 16

struct icon_entry {
    char name[40];
    int px, scale;
    uint32_t color;             /* ICON_COLOR_DEFAULT for the colour of the icon theme */
    int png;                    /* the PNG file, because the SVG file is missing */
    struct image *img;
};

/* The cache grows by doubling. */
static struct icon_entry *cache;
static int ncache, cache_cap;

static const char *icon_dir = "/usr/share/icons";

void icon_set_dir(const char *dir)
{
    icon_dir = dir;
}

static int device_scale(void)
{
    struct gui_output_info info;
    if (gui_output_count() > 0 && gui_get_output(0, &info) == 0 && info.scale > 0 && info.scale <= 4)
        return info.scale;
    return 1;
}

static uint32_t theme_color(const char *name)
{
    if (strcmp(name, "folder") == 0 || strcmp(name, "open") == 0)
        return 0x00d9a520;
    if (strcmp(name, "quit") == 0)
        return 0x00c04040;
    return 0x002a2a2a;
}

static struct image *load_svg(const char *name, int px, int scale, uint32_t color)
{
    char path[128];
    snprintf(path, sizeof path, "%s/%s.svg", icon_dir, name);
    struct image *img = image_load_svg(path, px * scale, color == ICON_COLOR_DEFAULT ? theme_color(name) : color);
    if (img)
        img->scale = scale;
    return img;
}

static struct icon_entry *find(const char *name, int px, int scale, uint32_t color, int png)
{
    for (int i = 0; i < ncache; i++) {
        struct icon_entry *e = &cache[i];
        if (e->px == px && e->scale == scale && e->color == color && e->png == png && strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

/* Adds an entry and returns img. Without memory, img is not cached. */
static const struct image *add(const char *name, int px, int scale, uint32_t color, int png, struct image *img)
{
    if (ncache == cache_cap) {
        int cap = cache_cap ? 2 * cache_cap : 64;
        struct icon_entry *grown = realloc(cache, (size_t)cap * sizeof *grown);
        if (!grown)
            return img;
        cache = grown;
        cache_cap = cap;
    }
    struct icon_entry *e = &cache[ncache++];
    memset(e, 0, sizeof *e);
    strlcpy(e->name, name, sizeof e->name);
    e->px = px;
    e->scale = scale;
    e->color = color;
    e->png = png;
    e->img = img;                       /* NULL is cached too */
    return img;
}

const struct image *icon_lookup(const char *name, int px, int scale, uint32_t color)
{
    if (scale < 1)
        scale = 1;
    struct icon_entry *e = find(name, px, scale, color, 0);
    if (e)
        return e->img;
    return add(name, px, scale, color, 0, load_svg(name, px, scale, color));
}

/* The SVG icon at the output scale, else the PNG file. */
static const struct image *lookup_default(const char *name, int px, int png_fallback)
{
    const struct image *img = icon_lookup(name, px, device_scale(), ICON_COLOR_DEFAULT);
    if (img || !png_fallback)
        return img;
    struct icon_entry *e = find(name, px, 1, ICON_COLOR_DEFAULT, 1);
    if (e)
        return e->img;
    char path[128];
    snprintf(path, sizeof path, "%s/%s.png", icon_dir, name);
    return add(name, px, 1, ICON_COLOR_DEFAULT, 1, image_load(path));
}

const struct image *icon_get(const char *name) { return lookup_default(name, ICON_PX, 1); }

const struct image *icon_get_size(const char *name, int px) { return lookup_default(name, px, 0); }

const struct image *icon_variant(const struct image *img, int scale, uint32_t color)
{
    if (!img)
        return NULL;
    for (int i = 0; i < ncache; i++) {
        struct icon_entry e = cache[i];
        if (e.img != img)
            continue;
        if (e.png || (e.scale == scale && e.color == color))
            return img;
        const struct image *v = icon_lookup(e.name, e.px, scale, color);
        return v ? v : img;
    }
    return img;
}

void painter_icon(struct painter *p, int x, int y, const struct image *img, int dimmed)
{
    if (!img)
        return;
    uint32_t color = ICON_COLOR_DEFAULT;
    for (int i = 0; i < ncache; i++)
        if (cache[i].img == img)
            color = cache[i].color;
    const struct image *v = icon_variant(img, p->scale, color);
    painter_image(p, x, y, dimmed ? icon_dimmed(v) : v);
}

/* Dimmed copies of icons for disabled buttons and menu items, created
 * once per icon. The copy has 40 percent of the alpha of the icon. */
static struct { const struct image *src; struct image *dim; } *dimmed;
static int ndimmed, dimmed_cap;

const struct image *icon_dimmed(const struct image *img)
{
    if (!img)
        return NULL;
    for (int i = 0; i < ndimmed; i++)
        if (dimmed[i].src == img)
            return dimmed[i].dim ? dimmed[i].dim : img;
    if (ndimmed == dimmed_cap) {
        int cap = dimmed_cap ? 2 * dimmed_cap : 64;
        void *grown = realloc(dimmed, (size_t)cap * sizeof *dimmed);
        if (!grown)
            return img;
        dimmed = grown;
        dimmed_cap = cap;
    }
    struct image *dim = image_create(img->w, img->h);
    if (dim) {
        dim->scale = img->scale;
        for (int k = 0; k < img->w * img->h; k++) {
            uint32_t v = img->pixels[k];
            dim->pixels[k] = ((v >> 24) * 2 / 5) << 24 | (v & 0x00ffffffu);
        }
    }
    dimmed[ndimmed].src = img;
    dimmed[ndimmed].dim = dim;
    ndimmed++;
    return dim ? dim : img;
}
