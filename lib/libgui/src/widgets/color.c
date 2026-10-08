/* The colour dialog and the colour button (gui/widget.h). */
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../intl.h"

/* ---- the colour dialog ---- */

/* The size of the field of saturation and value and the width of the hue
 * strip. */
static int field_w(const struct theme *t) { return theme_scale_px(t, 220); }
static int field_h(const struct theme *t) { return theme_scale_px(t, 160); }
static int strip_w(const struct theme *t) { return theme_scale_px(t, 24); }

struct color_state {
    struct app *app;
    struct widget *win, *field, *strip, *preview, *hex;
    int h, s, v;
    uint32_t old, rgb;
    int done, accepted;
    struct image field_img;             /* the field in device pixels, opaque */
    int field_hue;                      /* hue of field_img, -1 before the first paint */
};

/* The text field shows rgb unless it has the keyboard focus. */
static void show_hex(struct color_state *c)
{
    char text[16];
    snprintf(text, sizeof text, "#%06x", c->rgb & 0xffffff);
    if (!c->hex->focused)
        widget_set_text(c->hex, text);
}

static void set_hsv(struct color_state *c, int h, int s, int v)
{
    c->h = h;
    c->s = s < 0 ? 0 : s > 255 ? 255 : s;
    c->v = v < 0 ? 0 : v > 255 ? 255 : v;
    c->rgb = gfx_hsv_to_rgb(c->h, c->s, c->v);
    show_hex(c);
    widget_invalidate(c->field);
    widget_invalidate(c->strip);
    widget_invalidate(c->preview);
}

/* Saturation grows to the right, value grows upwards. A ring marks the
 * current colour. The field is computed at the device resolution of the
 * painter. A scale of 2 therefore shows no steps of 2 pixels. */
static int on_field_paint(struct widget *w, void *args, void *arg)
{
    struct color_state *c = arg;
    struct painter *p = ((struct sig_paint *)args)->p;
    int s = p->scale, fw = field_w(p->theme) * s, fh = field_h(p->theme) * s;
    if (c->field_img.scale != s) {
        free(c->field_img.pixels);
        c->field_img = (struct image){ fw, fh, malloc((size_t)fw * fh * 4), s };
        c->field_hue = -1;
    }
    if (!c->field_img.pixels)
        return 0;
    if (c->field_hue != c->h) {
        for (int y = 0; y < fh; y++)
            for (int x = 0; x < fw; x++)
                c->field_img.pixels[y * fw + x] =
                    0xff000000u | gfx_hsv_to_rgb(c->h, x * 255 / (fw - 1), 255 - y * 255 / (fh - 1));
        c->field_hue = c->h;
    }
    painter_image(p, 0, 0, &c->field_img);
    int x = c->s * (field_w(p->theme) - 1) / 255, y = (255 - c->v) * (field_h(p->theme) - 1) / 255;
    uint32_t ring = c->v > 128 ? 0x00000000 : 0x00ffffff;
    painter_frame(p, x - 4, y - 4, 9, 9, ring);
    painter_frame(p, 0, 0, w->w, w->h, p->theme->color[TC_BORDER]);
    return 1;
}

static int on_field_mouse(struct widget *w, void *args, void *arg)
{
    struct color_state *c = arg;
    struct sig_click *e = args;
    if (!(e->button & 1))
        return 1;
    const struct theme *t = widget_theme(w);
    set_hsv(c, c->h, e->x * 255 / (field_w(t) - 1), 255 - e->y * 255 / (field_h(t) - 1));
    return 1;
}

/* The hues from 0 at the top to 359 at the bottom, with a bar at the
 * current hue. */
static int on_strip_paint(struct widget *w, void *args, void *arg)
{
    struct color_state *c = arg;
    struct painter *p = ((struct sig_paint *)args)->p;
    for (int y = 0; y < w->h; y++)
        painter_fill(p, 0, y, w->w, 1, gfx_hsv_to_rgb(y * 359 / (w->h > 1 ? w->h - 1 : 1), 255, 255));
    int y = c->h * (w->h - 1) / 359;
    painter_fill(p, 0, y - 1, w->w, 3, 0x00000000);
    painter_frame(p, 0, 0, w->w, w->h, p->theme->color[TC_BORDER]);
    return 1;
}

static int on_strip_mouse(struct widget *w, void *args, void *arg)
{
    struct color_state *c = arg;
    struct sig_click *e = args;
    if (!(e->button & 1))
        return 1;
    int y = e->y < 0 ? 0 : e->y >= w->h ? w->h - 1 : e->y;
    set_hsv(c, y * 359 / (w->h > 1 ? w->h - 1 : 1), c->s, c->v);
    return 1;
}

/* The old colour on the left half, the new colour on the right half. */
static int on_preview_paint(struct widget *w, void *args, void *arg)
{
    struct color_state *c = arg;
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w / 2, w->h, c->old);
    painter_fill(p, w->w / 2, 0, w->w - w->w / 2, w->h, c->rgb);
    painter_frame(p, 0, 0, w->w, w->h, p->theme->color[TC_BORDER]);
    return 1;
}

/* A complete hexadecimal value moves the field and the strip to it. The
 * exact value is retained, not its rounded HSV form. */
static int on_hex_changed(struct widget *w, void *args, void *arg)
{
    struct color_state *c = arg;
    uint32_t rgb;
    if (gfx_color_parse(widget_text(w), &rgb) < 0)
        return 1;
    gfx_rgb_to_hsv(rgb, &c->h, &c->s, &c->v);
    c->rgb = rgb;
    widget_invalidate(c->field);
    widget_invalidate(c->strip);
    widget_invalidate(c->preview);
    return 1;
}

static int on_ok(struct widget *w, void *args, void *arg)
{
    struct color_state *c = arg;
    c->accepted = 1;
    c->done = 1;
    return 1;
}

static int on_cancel(struct widget *w, void *args, void *arg)
{
    ((struct color_state *)arg)->done = 1;
    return 1;
}

int color_dialog(struct app *a, struct widget *parent, const char *title, uint32_t *color)
{
    /* A modal dialog runs alone. The state is therefore static. */
    static struct color_state c;
    memset(&c, 0, sizeof c);
    c.app = a;
    c.old = c.rgb = *color & 0xffffff;
    c.field_hue = -1;
    gfx_rgb_to_hsv(c.rgb, &c.h, &c.s, &c.v);
    const struct theme *t = app_theme(a);
    int pad = theme_px(t, TM_PADDING);
    c.win = app_modal_window(a, parent ? parent : app_first_window(a), field_w(t) + strip_w(t) + 4 * pad,
                             field_h(t) + 2 * theme_px(t, TM_CONTROL_H) + 6 * pad, title);
    if (!c.win)
        return 0;
    struct widget *top = box_new(c.win, 0);
    c.field = canvas_new(top);
    widget_set_hint(c.field, field_w(t), field_h(t));
    widget_connect(c.field, "paint", on_field_paint, &c);
    widget_connect(c.field, "press", on_field_mouse, &c);
    widget_connect(c.field, "motion", on_field_mouse, &c);
    c.strip = canvas_new(top);
    widget_set_hint(c.strip, strip_w(t), field_h(t));
    widget_connect(c.strip, "paint", on_strip_paint, &c);
    widget_connect(c.strip, "press", on_strip_mouse, &c);
    widget_connect(c.strip, "motion", on_strip_mouse, &c);
    struct widget *mid = box_new(c.win, 0);
    c.preview = canvas_new(mid);
    widget_set_hint(c.preview, 64, 0);
    widget_connect(c.preview, "paint", on_preview_paint, &c);
    c.hex = textfield_new(mid, "");
    widget_set_stretch(c.hex, 1, 0);
    widget_connect(c.hex, "changed", on_hex_changed, &c);
    widget_connect(c.hex, "activate", on_ok, &c);
    show_hex(&c);
    struct widget *row = box_new(c.win, 0);
    struct widget *ok = button_new(row, _("OK"));
    struct widget *cancel = button_new(row, _("Cancel"));
    widget_set_stretch(ok, 1, 0);
    widget_set_stretch(cancel, 1, 0);
    widget_connect(ok, "clicked", on_ok, &c);
    widget_connect(cancel, "clicked", on_cancel, &c);
    widget_connect(c.win, "close", on_cancel, &c);
    widget_focus(ok);
    while (!c.done && app_step(a, -1))
        ;
    window_close(c.win);
    app_step(a, 0);
    free(c.field_img.pixels);
    c.field_img.pixels = NULL;
    if (c.accepted)
        *color = c.rgb;
    return c.accepted;
}

/* ---- the colour button ---- */

struct color_button {
    struct widget w;
    uint32_t color;
    char title[64];
};

static void color_button_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    h->pref_w = 120;
    h->min_w = 60;
    h->pref_h = h->min_h = theme_px(t, TM_CONTROL_H);
}

/* A swatch of the colour with its hexadecimal value. */
static void color_button_paint(struct widget *w, struct painter *p)
{
    struct color_button *b = (struct color_button *)w;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    painter_rounded(p, 0, 0, w->w, w->h, t->color[w->pressed ? TC_BUTTON_PRESSED : TC_BUTTON],
                    t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    int th = painter_text_height(p), sw = w->h - 8;
    painter_fill(p, 4, 4, sw, w->h - 8, b->color);
    painter_frame(p, 4, 4, sw, w->h - 8, t->color[TC_BORDER]);
    char text[16];
    snprintf(text, sizeof text, "#%06x", b->color & 0xffffff);
    painter_text(p, sw + 10, (w->h - th) / 2, text, t->color[TC_TEXT]);
}

static void color_button_choose(struct widget *w)
{
    struct color_button *b = (struct color_button *)w;
    uint32_t color = b->color;
    if (!color_dialog(w->app, w->window, b->title, &color) || color == b->color)
        return;
    b->color = color;
    widget_invalidate(w);
    struct sig_change ch = { (int)color, NULL };
    widget_emit(w, "changed", &ch);
}

/* The button acts on the release of the left button over it, as the
 * button class does. It shows the pressed state from the press to the
 * release. */
static int color_button_event(struct widget *w, struct event *e)
{
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (!(e->button & 1))
            return 0;
        w->pressed = 1;
        widget_capture(w);
        widget_invalidate(w);
        return 1;
    case EV_MOUSE_UP: {
        if (!w->pressed)
            return 0;
        w->pressed = 0;
        widget_invalidate(w);
        if (e->x >= 0 && e->y >= 0 && e->x < w->w && e->y < w->h)
            color_button_choose(w);
        return 1;
    }
    case EV_KEY_DOWN:
        if (e->ch == '\n' || e->ch == ' ') {
            color_button_choose(w);
            return 1;
        }
        return 0;
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

const struct widget_class color_button_class = { "colorbutton", sizeof(struct color_button), color_button_measure,
                                                 NULL, color_button_paint, color_button_event, NULL };

struct widget *colorbutton_new(struct widget *parent, uint32_t color, const char *title)
{
    struct widget *w = widget_new(&color_button_class, parent);
    if (!w)
        return NULL;
    struct color_button *b = (struct color_button *)w;
    w->focusable = 1;
    b->color = color & 0xffffff;
    snprintf(b->title, sizeof b->title, "%s", title ? title : "");
    return w;
}

uint32_t colorbutton_color(const struct widget *w)
{
    return ((const struct color_button *)w)->color;
}

void colorbutton_set(struct widget *w, uint32_t color)
{
    struct color_button *b = (struct color_button *)w;
    if (b->color == (color & 0xffffff))
        return;
    b->color = color & 0xffffff;
    widget_invalidate(w);
}
