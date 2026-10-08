/* The classes of full screen windows: spacer, backdrop, card and account
 * row. The greeter, the screen locker and askpass build their windows
 * from them (docs/design/widgets.md). */
#include <gui/app.h>
#include <gui/image.h>
#include <gui/wallpaper.h>
#include <minios/conf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- spacer ---- */

/* A spacer takes free space and paints nothing. */
static void spacer_measure(struct widget *w, struct size_hint *h) { (void)w; (void)h; }

const struct widget_class spacer_class = { "spacer", sizeof(struct widget), spacer_measure, NULL, NULL, NULL,
                                           NULL };

struct widget *spacer_new(struct widget *parent)
{
    struct widget *w = widget_new(&spacer_class, parent);
    if (w) {
        w->transparent = 1;
        widget_set_stretch(w, 1, 1);
    }
    return w;
}

/* ---- backdrop ---- */

/* The dimming is black at half opacity. */
#define BACKDROP_DIM_COLOR 0x80000000u

struct backdrop {
    struct widget w;
    int style;
    uint32_t color;             /* the desktop colour */
    struct image *wallpaper;
    struct image *rendered;     /* the wallpaper at the device size of the backdrop */
};

/* color with each channel scaled by percent. */
static uint32_t shade(uint32_t color, int percent)
{
    uint32_t r = ((color >> 16) & 0xff) * (uint32_t)percent / 100;
    uint32_t g = ((color >> 8) & 0xff) * (uint32_t)percent / 100;
    uint32_t b = (color & 0xff) * (uint32_t)percent / 100;
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static void backdrop_measure(struct widget *w, struct size_hint *h) { box_class.measure(w, h); }
static void backdrop_layout(struct widget *w) { box_class.layout(w); }

static void backdrop_paint(struct widget *w, struct painter *p)
{
    struct backdrop *b = (struct backdrop *)w;
    if (b->style == BACKDROP_DIM) {
        painter_fill(p, 0, 0, w->w, w->h, BACKDROP_DIM_COLOR);
        return;
    }
    if (b->wallpaper) {
        /* The wallpaper covers the backdrop and is cut at the longer side.
         * The backdrop renders the wallpaper again when its device size
         * changes. */
        if (b->rendered && (b->rendered->w != w->w * p->scale || b->rendered->h != w->h * p->scale)) {
            image_free(b->rendered);
            b->rendered = NULL;
        }
        if (!b->rendered)
            b->rendered = wallpaper_render(b->wallpaper, WALLPAPER_FILL, b->color, w->w, w->h, p->scale);
        if (b->rendered) {
            painter_image(p, 0, 0, b->rendered);
            return;
        }
    }
    for (int y = 0; y < w->h; y += 4)
        painter_fill(p, 0, y, w->w, 4, shade(b->color, 115 - 55 * y / (w->h ? w->h : 1)));
}

static void backdrop_destroy(struct widget *w)
{
    struct backdrop *b = (struct backdrop *)w;
    image_free(b->wallpaper);
    image_free(b->rendered);
}

const struct widget_class backdrop_class = { "backdrop", sizeof(struct backdrop), backdrop_measure,
                                             backdrop_layout, backdrop_paint, NULL, backdrop_destroy };

struct widget *backdrop_new(struct widget *window, int style)
{
    widget_set_padding(window, 0);
    struct widget *w = widget_new(&backdrop_class, window);
    if (!w)
        return NULL;
    struct backdrop *b = (struct backdrop *)w;
    w->value = 1;
    widget_set_stretch(w, 1, 1);
    b->style = style;
    b->color = 0x00306080;
    if (style == BACKDROP_DESKTOP) {
        char value[256];
        if (conf_lookup("/etc/desktop.conf", "desktop_color", value, sizeof value))
            b->color = (uint32_t)strtoul(value, NULL, 0) & 0xffffff;
        if (conf_lookup("/etc/desktop.conf", "wallpaper", value, sizeof value) && value[0])
            b->wallpaper = image_load(value);
    }
    return w;
}

/* ---- card ---- */

struct card {
    struct widget w;
    struct rect shown[3];       /* the opaque region set last */
};

static void card_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = widget_theme(w);
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_WINDOW], t->color[TC_BORDER]);
    struct window_state *ws = w->window ? window_state_of(w->window) : NULL;
    if (!ws || !ws->translucent || !ws->win)
        return;
    /* The opaque region is the card without its rounded corners: a band
     * of the full width and two narrower bands above and below it. */
    struct card *c = (struct card *)w;
    int x, y, r = theme_px(t, TM_RADIUS);
    widget_abs(w, &x, &y);
    if (2 * r > w->w || 2 * r > w->h)
        r = 0;
    struct rect region[3] = {
        { x, y + r, w->w, w->h - 2 * r },
        { x + r, y, w->w - 2 * r, r },
        { x + r, y + w->h - r, w->w - 2 * r, r },
    };
    if (memcmp(region, c->shown, sizeof region) != 0) {
        memcpy(c->shown, region, sizeof region);
        gui_set_opaque_region(ws->win, region, r ? 3 : 1);
    }
}

/* Escape in the card emits "cancel". */
static int card_event(struct widget *w, struct event *e)
{
    if (e->type == EV_KEY_DOWN && e->code == KEY_ESC)
        return widget_emit(w, "cancel", NULL);
    return 0;
}

const struct widget_class card_class = { "card", sizeof(struct card), backdrop_measure, backdrop_layout, card_paint,
                                         card_event, NULL };

struct widget *card_new(struct widget *parent, int width)
{
    struct widget *w = widget_new(&card_class, parent);
    if (!w)
        return NULL;
    w->value = 1;
    w->padding = 16;
    widget_set_align(w, ALIGN_CENTER, ALIGN_CENTER);
    widget_set_min(w, width, 0);
    widget_set_max(w, width, 0);
    return w;
}

/* ---- account row ---- */

struct account {
    struct widget w;
    char name[33];
    char full_name[64];
};

static void account_measure(struct widget *w, struct size_hint *h)
{
    struct account *a = (struct account *)w;
    const struct theme *t = widget_theme(w);
    int nw = widget_text_width(w, NULL, a->name, -1), fw = widget_text_width(w, NULL, a->full_name, -1);
    h->min_h = h->pref_h = theme_scale_px(t, ACCOUNT_ROW_H);
    h->pref_w = theme_scale_px(t, 8 + ACCOUNT_AVATAR + 12 + 8) + (nw > fw ? nw : fw);
}

static void account_paint(struct widget *w, struct painter *p)
{
    struct account *a = (struct account *)w;
    const struct theme *t = widget_theme(w);
    uint32_t text = t->color[TC_TEXT], dim = t->color[TC_TEXT_DISABLED];
    if (w->focusable && w->focused) {
        painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_SELECTION], t->color[TC_SELECTION]);
        text = dim = t->color[TC_SELECTION_TEXT];
    } else if (w->focusable && w->hover) {
        painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_BUTTON_HOVER], t->color[TC_BUTTON_HOVER]);
    }
    int pad = theme_scale_px(t, 8), size = theme_scale_px(t, ACCOUNT_AVATAR), gap = theme_scale_px(t, 4);
    int fh = theme_px(t, TM_FONT_PX);
    painter_avatar(p, pad, (w->h - size) / 2, size, a->name, a->full_name);
    int tx = pad + size + theme_scale_px(t, 12), ty = (w->h - 2 * fh - gap) / 2;
    painter_text(p, tx, ty, a->full_name, text);
    painter_text(p, tx, ty + fh + gap, a->name, dim);
    if (w->focused)
        painter_focus_ring(p, 0, 0, w->w, w->h);
}

/* The next focusable account row after w, or before w for dir -1. */
static struct widget *account_step(struct widget *w, int dir)
{
    struct widget *o = dir > 0 ? w->next : w->prev;
    while (o && !(o->cls == &account_class && o->focusable && o->visible))
        o = dir > 0 ? o->next : o->prev;
    return o;
}

static int account_event(struct widget *w, struct event *e)
{
    if (!w->focusable)
        return 0;
    if (e->type == EV_MOUSE_UP && e->x >= 0 && e->y >= 0 && e->x < w->w && e->y < w->h) {
        widget_emit(w, "clicked", NULL);
        return 1;
    }
    if (e->type == EV_KEY_DOWN && (e->code == KEY_ENTER || e->code == KEY_SPACE)) {
        widget_emit(w, "clicked", NULL);
        return 1;
    }
    if (e->type == EV_KEY_DOWN && (e->code == KEY_UP || e->code == KEY_DOWN)) {
        struct widget *o = account_step(w, e->code == KEY_DOWN ? 1 : -1);
        if (o)
            widget_focus(o);
        return 1;
    }
    if (e->type == EV_ENTER || e->type == EV_LEAVE || e->type == EV_FOCUS_IN || e->type == EV_FOCUS_OUT)
        widget_invalidate(w);
    return 0;
}

const struct widget_class account_class = { "account", sizeof(struct account), account_measure, NULL,
                                            account_paint, account_event, NULL };

struct widget *account_new(struct widget *parent, const char *name, const char *full_name, int focusable)
{
    struct widget *w = widget_new(&account_class, parent);
    if (w) {
        w->focusable = focusable ? 1 : 0;
        account_set(w, name, full_name);
    }
    return w;
}

void account_set(struct widget *w, const char *name, const char *full_name)
{
    struct account *a = (struct account *)w;
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->full_name, sizeof a->full_name, "%s", full_name && full_name[0] ? full_name : name);
    widget_relayout(w);
}

const char *account_name(const struct widget *w)
{
    return ((const struct account *)w)->name;
}
