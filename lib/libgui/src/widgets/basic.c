/* Labels, buttons, check boxes, radio buttons, separators, canvases. */
#include <gui/app.h>
#include <gui/utf8.h>
#include <stdio.h>
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

/* The flags of a label in w->value. */
#define LABEL_WRAP 1
#define LABEL_ELLIPSIS 2

/* The lines of a wrapped label at the width width. */
static int label_lines(struct widget *w, int width, int *start, int *len, int max)
{
    struct surface none = { NULL, 0, 0, 0 };
    struct painter p;
    painter_init_scaled(&p, &none, widget_theme(w), widget_scale(w));
    return painter_wrap(&p, widget_text(w), width > 1 ? width : 1, start, len, max);
}

/* A wrapped label: the lines of painter_wrap, one below the other. */
static void label_paint_wrapped(struct widget *w, struct painter *p, uint32_t color)
{
    int start[64], len[64], th = painter_text_height(p);
    int n = painter_wrap(p, widget_text(w), w->w > 1 ? w->w : 1, start, len, 64);
    for (int i = 0; i < n && i < 64; i++) {
        char line[512];
        snprintf(line, sizeof line, "%.*s", len[i], widget_text(w) + start[i]);
        painter_text(p, 0, i * th, line, color);
    }
}

/* The longest prefix of the text that fits into the width with an
 * ellipsis, then the ellipsis. */
static void label_paint_ellipsis(struct widget *w, struct painter *p, int x, int y, uint32_t color)
{
    const char *text = widget_text(w);
    int avail = w->w - x;
    if (painter_text_width(p, text, -1) <= avail) {
        painter_text(p, x, y, text, color);
        return;
    }
    int n = (int)strlen(text), cut = painter_text_index(p, text, n, avail - painter_text_width(p, "\u2026", -1));
    while (cut > 0 && painter_text_width(p, text, cut) + painter_text_width(p, "\u2026", -1) > avail)
        cut = gui_utf8_prev_boundary(text, cut);
    char shown[512];
    snprintf(shown, sizeof shown, "%.*s\u2026", cut, text);
    painter_text(p, x, y, shown, color);
}

static void label_paint(struct widget *w, struct painter *p)
{
    uint32_t color = p->theme->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED];
    if (w->value & LABEL_WRAP) {
        label_paint_wrapped(w, p, color);
        return;
    }
    int x = 0;
    if (w->icon) {
        painter_icon(p, 0, (w->h - image_lh(w->icon)) / 2, w->icon, 0);
        x = image_lw(w->icon) + 4;
    }
    int y = (w->h - painter_text_height(p)) / 2;
    if (w->value & LABEL_ELLIPSIS)
        label_paint_ellipsis(w, p, x, y, color);
    else
        painter_mnemonic_text(p, x, y, widget_text(w), color);
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
    int fh = widget_theme(w)->font->height;
    if (w->value & LABEL_ELLIPSIS)
        h->min_w = 2 * fh;
    if (w->value & LABEL_WRAP) {
        /* The height for the current width, or for the preferred width
         * before the first layout. The layout measures again when the
         * width changes the number of lines. */
        int width = w->w > 0 ? w->w : w->hint.pref_w > 0 ? w->hint.pref_w : 300;
        int start[1], len[1];
        h->min_w = 4 * fh;
        h->pref_w = width < h->pref_w ? width : h->pref_w;
        h->pref_h = h->min_h = label_lines(w, width, start, len, 0) * fh;
    }
}

/* A wrapped label measures its height for its width. A new width that
 * changes the number of lines measures the label again. */
static void label_layout(struct widget *w)
{
    int start[1], len[1], fh = widget_theme(w)->font->height;
    if ((w->value & LABEL_WRAP) && label_lines(w, w->w, start, len, 0) * fh != w->measured.pref_h)
        widget_relayout(w);
}

const struct widget_class label_class = { "label", sizeof(struct widget), label_measure, label_layout, label_paint, NULL,
                                          NULL };

void label_set_wrap(struct widget *w, int wrap)
{
    w->value = wrap ? w->value | LABEL_WRAP : w->value & ~LABEL_WRAP;
    widget_relayout(w);
}

void label_set_ellipsis(struct widget *w, int ellipsis)
{
    w->value = ellipsis ? w->value | LABEL_ELLIPSIS : w->value & ~LABEL_ELLIPSIS;
    widget_relayout(w);
}

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
    painter_button(p, 0, 0, w->w, w->h, widget_paint_state(w));
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

/* The box of a check box and the disc of a radio button, TM_ICON pixels
 * wide. An unchecked box is a field with a border, which turns to the
 * accent colour under the pointer. A checked box is filled with the
 * accent colour and shows a check mark or a dot in the selection text
 * colour. A disabled box uses the track colour and the disabled text
 * colour. */
static void check_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = p->theme;
    int box = theme_px(t, TM_ICON);
    int by = (w->h - box) / 2;
    int radio = w->cls == &radio_class;
    uint32_t fill = !w->enabled ? t->color[TC_TRACK] : w->value ? t->color[TC_ACCENT] : t->color[TC_FIELD];
    uint32_t border = !w->enabled ? t->color[TC_BORDER] : w->value || w->hover ? t->color[TC_ACCENT] : t->color[TC_BORDER];
    uint32_t mark = w->enabled ? t->color[TC_SELECTION_TEXT] : t->color[TC_TEXT_DISABLED];
    if (radio) {
        painter_disc(p, 0, by, box, border);
        painter_disc(p, 1, by + 1, box - 2, fill);
        if (w->value) {
            int dot = box * 3 / 8;
            painter_disc(p, (box - dot) / 2, by + (box - dot) / 2, dot, mark);
        }
    } else {
        painter_round_rect(p, 0, by, box, box, theme_scale_px(t, 3), fill, border);
        if (w->value)
            painter_check(p, 0, by, box, mark);
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
    if (e->type == EV_ENTER || e->type == EV_LEAVE) {
        widget_invalidate(w);
        return 1;
    }
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
    h->pref_w += theme_px(widget_theme(w), TM_ICON) + 6;
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

/* ---- switch ---- */

/* A switch is an on and off control. value is 1 while on. A click, Space
 * or Enter toggles it and emits "toggled". */
static void switch_measure(struct widget *w, struct size_hint *h)
{
    h->min_w = h->pref_w = PAINTER_SWITCH_W + 4;
    h->min_h = h->pref_h = PAINTER_SWITCH_H + 4;
}

static void switch_paint(struct widget *w, struct painter *p)
{
    painter_switch(p, 2, (w->h - PAINTER_SWITCH_H) / 2, w->value, widget_paint_state(w));
}

const struct widget_class switch_class = { "switch", sizeof(struct widget), switch_measure, NULL, switch_paint,
                                           check_event, NULL };

struct widget *switch_new(struct widget *parent)
{
    struct widget *w = widget_new(&switch_class, parent);
    if (w) {
        w->focusable = 1;
        w->transparent = 1;
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
