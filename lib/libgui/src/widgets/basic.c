/* Labels, buttons, check boxes, radio buttons, separators, canvases. */
#include <gui/app.h>
#include <stdlib.h>
#include <string.h>

/* The width of the caption without the mnemonic marker. */
static int caption_width(const struct widget *w)
{
    char buf[256];
    painter_mnemonic_strip(widget_text(w), buf, sizeof buf);
    return buf[0] ? widget_text_width(w, NULL, buf, -1) : 0;
}

static void text_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    int tw = caption_width(w);
    h->pref_w = tw + 2 * theme_px(t, TM_PADDING);
    if (w->icon)
        h->pref_w += image_lw(w->icon) + (tw ? 4 : 0);
    h->pref_h = theme_px(t, TM_CONTROL_H);
    if (w->icon && image_lh(w->icon) + 8 > h->pref_h)
        h->pref_h = image_lh(w->icon) + 8;
    h->min_w = h->pref_w;
    h->min_h = t->font->height;
}

/* ---- label ---- */

static void label_paint(struct widget *w, struct painter *p)
{
    if (w->value == 1) {                /* tooltip style */
        painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_HIGHLIGHT]);
        painter_frame(p, 0, 0, w->w, w->h, p->theme->color[TC_BORDER]);
        painter_text(p, 4, 3, widget_text(w), p->theme->color[TC_TEXT]);
        return;
    }
    int x = 0;
    if (w->icon) {
        painter_icon(p, 0, (w->h - image_lh(w->icon)) / 2, w->icon, 0);
        x = image_lw(w->icon) + 4;
    }
    int y = (w->h - painter_text_height(p)) / 2;
    painter_mnemonic_text(p, x, y, widget_text(w), p->theme->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
}

static void label_measure(struct widget *w, struct size_hint *h)
{
    text_measure(w, h);
    h->pref_w -= 2 * theme_px(widget_theme(w), TM_PADDING);
    if (w->icon)
        h->pref_w += image_lw(w->icon) + 4;
    h->min_w = h->pref_w;
    h->pref_h = widget_theme(w)->font->height + 2;
    if (w->icon && image_lh(w->icon) > h->pref_h)
        h->pref_h = image_lh(w->icon);
}

const struct widget_class label_class = { "label", sizeof(struct widget), label_measure, NULL, label_paint, NULL, NULL };

struct widget *label_new(struct widget *parent, const char *text)
{
    struct widget *w = widget_new(&label_class, parent);
    if (w) {
        w->transparent = 1;
        widget_set_text(w, text);
    }
    return w;
}

/* ---- button ---- */

static void button_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    uint32_t fill = !w->enabled ? t->color[TC_TRACK] : w->pressed ? t->color[TC_BUTTON_PRESSED]
                    : w->hover ? t->color[TC_BUTTON_HOVER] : t->color[TC_BUTTON];
    painter_rounded(p, 0, 0, w->w, w->h, fill, 0xffffffffu);
    char buf[256];
    painter_mnemonic_strip(widget_text(w), buf, sizeof buf);
    int tw = buf[0] ? painter_text_width(p, buf, -1) : 0;
    int iw = w->icon ? image_lw(w->icon) + (buf[0] ? 4 : 0) : 0;
    int x = (w->w - tw - iw) / 2;
    if (w->icon) {
        painter_icon(p, x, (w->h - image_lh(w->icon)) / 2, w->icon, !w->enabled);
        x += iw;
    }
    if (buf[0])
        painter_mnemonic_text(p, x, (w->h - painter_text_height(p)) / 2, widget_text(w),
                              t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    if (w->focused)
        painter_focus_ring(p, 2, 2, w->w - 4, w->h - 4);
}

static int button_event(struct widget *w, struct event *e)
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
        int inside = e->x >= 0 && e->y >= 0 && e->x < w->w && e->y < w->h;
        if (inside) {
            struct sig_click c = { 1, e->x, e->y };
            widget_emit(w, "clicked", &c);
        }
        return 1;
    }
    case EV_KEY_DOWN:
        if (e->ch == '\n' || e->ch == ' ') {
            struct sig_click c = { 1, w->w / 2, w->h / 2 };
            widget_emit(w, "clicked", &c);
            return 1;
        }
        return 0;
    case EV_ENTER: case EV_LEAVE:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

const struct widget_class button_class = { "button", sizeof(struct widget), text_measure, NULL, button_paint, button_event, NULL };

struct widget *button_new(struct widget *parent, const char *text)
{
    struct widget *w = widget_new(&button_class, parent);
    if (w) {
        w->focusable = 1;
        w->transparent = 1;
        widget_set_text(w, text);
    }
    return w;
}

/* ---- check box and radio button ---- */

static void check_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    int box = painter_text_height(p) - 2;
    if (box < 10) box = 10;
    int by = (w->h - box) / 2;
    int radio = w->cls == &radio_class;
    if (radio)
        painter_rounded(p, 0, by, box, box, t->color[TC_FIELD], t->color[TC_BORDER]);
    else
        painter_frame(p, 0, by, box, box, t->color[TC_BORDER]), painter_fill(p, 1, by + 1, box - 2, box - 2, t->color[TC_FIELD]);
    if (w->value) {
        if (radio)
            painter_fill(p, 3, by + 3, box - 6, box - 6, t->color[TC_ACCENT]);
        else {
            painter_line(p, 3, by + box / 2, box / 2, by + box - 4, t->color[TC_TEXT]);
            painter_line(p, box / 2, by + box - 4, box - 3, by + 3, t->color[TC_TEXT]);
        }
    }
    painter_mnemonic_text(p, box + 6, (w->h - painter_text_height(p)) / 2, widget_text(w),
                          t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    if (w->focused)
        painter_focus_ring(p, 0, 0, w->w, w->h);
}

static void toggle(struct widget *w)
{
    if (w->cls == &radio_class) {
        if (w->value)
            return;
        if (w->parent)
            for (struct widget *s = w->parent->first; s; s = s->next)
                if (s != w && s->cls == &radio_class && s->value) {
                    s->value = 0;
                    widget_invalidate(s);
                    struct sig_change c = { 0, widget_text(s) };
                    widget_emit(s, "toggled", &c);
                }
        w->value = 1;
    } else {
        w->value = !w->value;
    }
    widget_invalidate(w);
    struct sig_change c = { w->value, widget_text(w) };
    widget_emit(w, "toggled", &c);
}

static int check_event(struct widget *w, struct event *e)
{
    if (e->type == EV_MOUSE_DOWN && (e->button & 1)) {
        toggle(w);
        return 1;
    }
    if (e->type == EV_KEY_DOWN && (e->ch == ' ' || e->ch == '\n')) {
        toggle(w);
        return 1;
    }
    return 0;
}

static void check_measure(struct widget *w, struct size_hint *h)
{
    text_measure(w, h);
    h->pref_w += widget_theme(w)->font->height + 6;
    h->min_w = h->pref_w;
}

const struct widget_class checkbox_class = { "checkbox", sizeof(struct widget), check_measure, NULL, check_paint, check_event, NULL };
const struct widget_class radio_class = { "radio", sizeof(struct widget), check_measure, NULL, check_paint, check_event, NULL };

struct widget *checkbox_new(struct widget *parent, const char *text)
{
    struct widget *w = widget_new(&checkbox_class, parent);
    if (w) {
        w->focusable = 1;
        w->transparent = 1;
        widget_set_text(w, text);
    }
    return w;
}

struct widget *radio_new(struct widget *parent, const char *text)
{
    struct widget *w = widget_new(&radio_class, parent);
    if (w) {
        w->focusable = 1;
        w->transparent = 1;
        widget_set_text(w, text);
    }
    return w;
}

/* ---- separator ---- */

static void separator_measure(struct widget *w, struct size_hint *h)
{
    h->pref_w = h->pref_h = 2;
    h->min_w = h->min_h = 2;
}

static void separator_paint(struct widget *w, struct painter *p)
{
    if (w->w >= w->h)
        painter_line(p, 0, w->h / 2, w->w - 1, w->h / 2, p->theme->color[TC_BORDER]);
    else
        painter_line(p, w->w / 2, 0, w->w / 2, w->h - 1, p->theme->color[TC_BORDER]);
}

const struct widget_class separator_class = { "separator", sizeof(struct widget), separator_measure, NULL, separator_paint, NULL, NULL };

struct widget *separator_new(struct widget *parent)
{
    struct widget *w = widget_new(&separator_class, parent);
    if (w)
        w->transparent = 1;
    return w;
}

/* ---- canvas ---- */

static void canvas_measure(struct widget *w, struct size_hint *h)
{
    h->pref_w = h->pref_h = 40;
}

static void canvas_paint(struct widget *w, struct painter *p)
{
    struct sig_paint s = { p };
    if (!widget_emit(w, "paint", &s))
        painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_FIELD]);
}

static int canvas_event(struct widget *w, struct event *e)
{
    switch (e->type) {
    case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_MOVE: case EV_MOUSE_WHEEL: {
        struct sig_click c = { e->button, e->x, e->y };
        const char *name = e->type == EV_MOUSE_DOWN ? "press" : e->type == EV_MOUSE_UP ? "release"
                           : e->type == EV_MOUSE_MOVE ? "motion" : "wheel";
        if (e->type == EV_MOUSE_DOWN)
            widget_capture(w);
        return widget_emit(w, name, &c);
    }
    case EV_KEY_DOWN: case EV_KEY_UP: {
        struct sig_key k = { e->code, e->ch, e->mods };
        return widget_emit(w, e->type == EV_KEY_DOWN ? "key" : "keyup", &k);
    }
    case EV_TEXT: case EV_PREEDIT: {
        struct sig_text t = { e->text ? e->text : "" };
        return widget_emit(w, e->type == EV_TEXT ? "text" : "preedit", &t);
    }
    case EV_DRAG_MOVE: case EV_DROP: case EV_DRAG_LEAVE: case EV_DRAG_END: {
        struct sig_drag d = { -1, e->x, e->y, e->drag };
        const char *name = e->type == EV_DRAG_MOVE ? "drag_motion" : e->type == EV_DROP ? "drop"
                           : e->type == EV_DRAG_LEAVE ? "drag_leave" : "drag_end";
        return widget_emit(w, name, &d);
    }
    default:
        return 0;
    }
}

const struct widget_class canvas_class = { "canvas", sizeof(struct widget), canvas_measure, NULL, canvas_paint, canvas_event, NULL };

struct widget *canvas_new(struct widget *parent)
{
    struct widget *w = widget_new(&canvas_class, parent);
    if (w)
        widget_set_stretch(w, 1, 1);
    return w;
}
