/* The background, the top bar, the card and the account rows of the
 * greeter and of the screen locker (screen.h). */
#include "screen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gui/image.h>
#include <gui/theme.h>
#include <gui/wallpaper.h>
#include <minios/conf.h>

static uint32_t desktop_color = 0x00306080;
static struct image *wallpaper;
/* The wallpaper at the device size of the screen (wallpaper_render). */
static struct image *backdrop;
static struct widget *clock_label;

void screen_load_background(void)
{
    char value[256];
    if (conf_lookup("/etc/desktop.conf", "desktop_color", value, sizeof value))
        desktop_color = (uint32_t)strtoul(value, NULL, 0) & 0xffffff;
    if (conf_lookup("/etc/desktop.conf", "wallpaper", value, sizeof value) && value[0])
        wallpaper = image_load(value);
}

/* color with each channel scaled by percent. */
static uint32_t shade(uint32_t color, int percent)
{
    uint32_t r = ((color >> 16) & 0xff) * (uint32_t)percent / 100;
    uint32_t g = ((color >> 8) & 0xff) * (uint32_t)percent / 100;
    uint32_t b = (color & 0xff) * (uint32_t)percent / 100;
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

/* The backdrop is a vertical box that paints the background. */
static void box_measure(struct widget *w, struct size_hint *h) { box_class.measure(w, h); }
static void box_layout(struct widget *w) { box_class.layout(w); }

static void backdrop_paint(struct widget *w, struct painter *p)
{
    if (wallpaper) {
        /* The wallpaper covers the screen and is cut at the longer side.
         * The backdrop renders the wallpaper at the device size of the
         * screen and renders it again when that size changes. */
        if (backdrop && (backdrop->w != w->w * p->scale || backdrop->h != w->h * p->scale)) {
            image_free(backdrop);
            backdrop = NULL;
        }
        if (!backdrop)
            backdrop = wallpaper_render(wallpaper, WALLPAPER_FILL, desktop_color, w->w, w->h, p->scale);
        if (backdrop) {
            painter_image(p, 0, 0, backdrop);
            return;
        }
    }
    for (int y = 0; y < w->h; y += 4)
        painter_fill(p, 0, y, w->w, 4, shade(desktop_color, 115 - 55 * y / (w->h ? w->h : 1)));
}

static const struct widget_class backdrop_class = { "screen-backdrop", sizeof(struct widget), box_measure,
                                                    box_layout, backdrop_paint, NULL, NULL };

/* The top bar and the card are boxes with a background of the theme. */
static void bar_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = widget_theme(w);
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    painter_fill(p, 0, w->h - 1, w->w, 1, t->color[TC_BORDER]);
}

static void card_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = widget_theme(w);
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_WINDOW], t->color[TC_BORDER]);
}

/* The clock is centred on the whole bar, regardless of the widths of the host
 * name on its left and of the buttons on its right. */
static void bar_layout(struct widget *w)
{
    box_class.layout(w);
    if (clock_label && clock_label->parent == w)
        clock_label->x = (w->w - clock_label->w) / 2;
}

static const struct widget_class bar_class = { "screen-bar", sizeof(struct widget), box_measure, bar_layout,
                                               bar_paint, NULL, NULL };
/* A box that paints nothing, for the buttons beside the centred clock. */
static const struct widget_class clear_box_class = { "screen-box", sizeof(struct widget), box_measure, box_layout,
                                                     NULL, NULL, NULL };
static const struct widget_class card_class = { "screen-card", sizeof(struct widget), box_measure, box_layout,
                                                card_paint, NULL, NULL };

/* A spacer takes the free space of the backdrop and paints nothing. */
static void spacer_measure(struct widget *w, struct size_hint *h) { (void)w; (void)h; }

static const struct widget_class spacer_class = { "screen-spacer", sizeof(struct widget), spacer_measure, NULL,
                                                  NULL, NULL, NULL };

static struct widget *container_new(const struct widget_class *cls, struct widget *parent, int vertical, int padding)
{
    struct widget *w = widget_new(cls, parent);
    if (w) {
        w->value = vertical;
        w->padding = padding;
    }
    return w;
}

struct widget *screen_backdrop_new(struct widget *window)
{
    widget_set_padding(window, 0);
    struct widget *back = container_new(&backdrop_class, window, 1, 0);
    widget_set_stretch(back, 1, 1);
    return back;
}

struct widget *screen_bar_new(struct widget *backdrop, const char *host)
{
    struct widget *bar = container_new(&bar_class, backdrop, 0, 4);
    struct widget *host_label = label_new(bar, host);
    widget_set_stretch(host_label, 1, 0);
    clock_label = label_new(bar, "");
    struct widget *right = container_new(&clear_box_class, bar, 0, 0);
    widget_set_stretch(right, 1, 0);
    /* A transparent spacer. The centred clock remains visible. */
    struct widget *gap = widget_new(&spacer_class, right);
    widget_set_stretch(gap, 1, 0);
    return right;
}

static void tick(void *arg)
{
    char text[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(text, sizeof text, "%a %d %b  %H:%M", &tm);
    widget_set_text(clock_label, text);
}

void screen_clock_start(struct app *a)
{
    tick(NULL);
    app_timer_add(a, 10000, 1, tick, NULL);
}

struct widget *screen_card_new(struct widget *backdrop)
{
    struct widget *top = widget_new(&spacer_class, backdrop);
    widget_set_stretch(top, 0, 1);
    struct widget *card = container_new(&card_class, backdrop, 1, 16);
    widget_set_align(card, ALIGN_CENTER, ALIGN_CENTER);
    widget_set_min(card, SCREEN_CARD_W, 0);
    widget_set_max(card, SCREEN_CARD_W, 0);
    struct widget *bottom = widget_new(&spacer_class, backdrop);
    widget_set_stretch(bottom, 0, 2);
    return card;
}

void screen_paint_account(struct painter *p, int x, int y, int h, const char *name, const char *full_name,
                          uint32_t text, uint32_t dim)
{
    int fh = p->theme->metric[TM_FONT_PX];
    painter_avatar(p, x + 8, y + (h - SCREEN_AVATAR) / 2, SCREEN_AVATAR, name, full_name);
    int tx = x + 8 + SCREEN_AVATAR + 12, ty = y + (h - 2 * fh - 4) / 2;
    painter_text(p, tx, ty, full_name, text);
    painter_text(p, tx, ty + fh + 4, name, dim);
}

/* The account row stores its two names in the widget text, separated by a
 * newline: the account name first, then the full name. */
static void account_measure(struct widget *w, struct size_hint *h)
{
    h->min_h = h->pref_h = SCREEN_ROW_H;
    h->min_w = h->pref_w = SCREEN_CARD_W - 40;
}

static void account_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = widget_theme(w);
    const char *text = widget_text(w), *nl = strchr(text, '\n');
    char name[64];
    snprintf(name, sizeof name, "%.*s", nl ? (int)(nl - text) : (int)strlen(text), text);
    screen_paint_account(p, 0, 0, w->h, name, nl ? nl + 1 : name, t->color[TC_TEXT], t->color[TC_TEXT_DISABLED]);
}

static const struct widget_class account_class = { "screen-account", sizeof(struct widget), account_measure, NULL,
                                                   account_paint, NULL, NULL };

struct widget *screen_account_new(struct widget *parent, const char *name, const char *full_name)
{
    struct widget *w = widget_new(&account_class, parent);
    if (w) {
        char text[160];
        snprintf(text, sizeof text, "%s\n%s", name, full_name);
        widget_set_text(w, text);
    }
    return w;
}
