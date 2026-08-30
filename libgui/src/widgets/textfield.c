/* Single line text field with cursor, selection and the clipboard. */
#include <gui/app.h>
#include <stdlib.h>
#include <string.h>

#define PAD 4

struct textfield {
    struct widget w;
    int cursor, sel;            /* sel: anchor, -1 none */
    int scroll_x;
};

static void changed(struct textfield *f)
{
    struct sig_change c = { 0, widget_text(&f->w) };
    widget_invalidate(&f->w);
    widget_emit(&f->w, "changed", &c);
}

static int len_of(struct textfield *f) { return (int)strlen(widget_text(&f->w)); }

static void clamp(struct textfield *f)
{
    int n = len_of(f);
    if (f->cursor > n) f->cursor = n;
    if (f->cursor < 0) f->cursor = 0;
    if (f->sel > n) f->sel = n;
}

static void sel_range(struct textfield *f, int *a, int *b)
{
    *a = f->sel < f->cursor ? f->sel : f->cursor;
    *b = f->sel < f->cursor ? f->cursor : f->sel;
}

static int delete_selection(struct textfield *f)
{
    if (f->sel < 0 || f->sel == f->cursor) {
        f->sel = -1;
        return 0;
    }
    int a, b;
    sel_range(f, &a, &b);
    char *t = f->w.text;
    memmove(t + a, t + b, strlen(t + b) + 1);
    f->cursor = a;
    f->sel = -1;
    return 1;
}

static void insert(struct textfield *f, const char *s, int n)
{
    int len = len_of(f);
    char *t = realloc(f->w.text, (size_t)(len + n + 1));
    if (!t)
        return;
    f->w.text = t;
    memmove(t + f->cursor + n, t + f->cursor, (size_t)(len - f->cursor + 1));
    memcpy(t + f->cursor, s, (size_t)n);
    f->cursor += n;
}

static void scroll_to_cursor(struct textfield *f, const struct theme *t)
{
    int cx = gfx_text_width_font(t->font, widget_text(&f->w), f->cursor);
    int avail = f->w.w - 2 * PAD;
    if (cx < f->scroll_x) f->scroll_x = cx;
    if (cx > f->scroll_x + avail - 1) f->scroll_x = cx - avail + 1;
    if (f->scroll_x < 0) f->scroll_x = 0;
}

static void textfield_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    h->pref_w = 120;
    h->min_w = 40;
    h->pref_h = h->min_h = theme_px(t, TM_CONTROL_H);
}

static void textfield_paint(struct widget *w, struct painter *p)
{
    struct textfield *f = (struct textfield *)w;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_FIELD], t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    painter_push(p, 1, 1, w->w - 2, w->h - 2);
    const char *text = widget_text(w);
    int th = painter_text_height(p);
    int ty = (w->h - 2 - th) / 2, tx = PAD - 1 - f->scroll_x;
    if (f->sel >= 0 && f->sel != f->cursor) {
        int a, b;
        sel_range(f, &a, &b);
        int x0 = painter_text_width(p, text, a), x1 = painter_text_width(p, text, b);
        painter_fill(p, tx + x0, ty - 1, x1 - x0, th + 2, t->color[TC_SELECTION]);
    }
    painter_text(p, tx, ty, text, t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    if (w->focused) {
        int cx = tx + painter_text_width(p, text, f->cursor);
        painter_line(p, cx, ty - 1, cx, ty + th, t->color[TC_TEXT]);
    }
    painter_pop(p);
}

static int pos_at(struct textfield *f, int px)
{
    const struct theme *t = widget_theme(&f->w);
    int rel = px - PAD + f->scroll_x;
    return gfx_text_index_font(t->font, widget_text(&f->w), -1, rel < 0 ? 0 : rel);
}

static int key(struct textfield *f, struct event *e)
{
    const struct theme *t = widget_theme(&f->w);
    int ctrl = e->mods & WMOD_CTRL, shift = e->mods & WMOD_SHIFT;
    int before = f->cursor, len = len_of(f);
    if (e->mods & WMOD_ALT)
        return 0;                            /* mnemonics and accelerators */
    int moved = 1;
    if (ctrl && e->ch == 1) {                    /* Ctrl+A */
        f->sel = 0;
        f->cursor = len;
        widget_invalidate(&f->w);
        return 1;
    }
    if (ctrl && (e->ch == 3 || e->ch == 24)) {   /* Ctrl+C, Ctrl+X */
        if (f->sel >= 0 && f->sel != f->cursor) {
            int a, b;
            sel_range(f, &a, &b);
            gui_clipboard_set(widget_text(&f->w) + a, b - a);
            if (e->ch == 24) {
                delete_selection(f);
                changed(f);
            }
        }
        return 1;
    }
    if (ctrl && e->ch == 22) {                   /* Ctrl+V */
        char *tmp = malloc(WSRV_CLIP_MAX);
        int n = tmp ? gui_clipboard_get(tmp, WSRV_CLIP_MAX) : -1;
        if (n > 0) {
            for (int i = 0; i < n; i++)
                if (tmp[i] == '\n')
                    tmp[i] = ' ';
            delete_selection(f);
            insert(f, tmp, n);
            changed(f);
        }
        free(tmp);
        scroll_to_cursor(f, t);
        return 1;
    }
    switch (e->code) {
    case 0xcb: if (f->cursor > 0) f->cursor--; break;
    case 0xcd: if (f->cursor < len) f->cursor++; break;
    case 0xc7: f->cursor = 0; break;
    case 0xcf: f->cursor = len; break;
    default: moved = 0;
    }
    if (moved) {
        if (shift) {
            if (f->sel < 0)
                f->sel = before;
        } else {
            f->sel = -1;
        }
        scroll_to_cursor(f, t);
        widget_invalidate(&f->w);
        return 1;
    }
    if (e->code == 0xd3) {                       /* Delete */
        if (!delete_selection(f) && f->cursor < len)
            memmove(f->w.text + f->cursor, f->w.text + f->cursor + 1, (size_t)(len - f->cursor));
    } else if (e->ch == '\b') {
        if (!delete_selection(f) && f->cursor > 0) {
            memmove(f->w.text + f->cursor - 1, f->w.text + f->cursor, (size_t)(len - f->cursor + 1));
            f->cursor--;
        }
    } else if (e->ch == '\n') {
        struct sig_change c = { 0, widget_text(&f->w) };
        widget_emit(&f->w, "activate", &c);
        return 1;
    } else if (e->ch >= 32 && e->ch < 127 && !ctrl) {
        char ch = (char)e->ch;
        delete_selection(f);
        insert(f, &ch, 1);
    } else {
        return 0;
    }
    changed(f);
    scroll_to_cursor(f, t);
    return 1;
}

static int textfield_event(struct widget *w, struct event *e)
{
    struct textfield *f = (struct textfield *)w;
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (!(e->button & 1))
            return 0;
        f->cursor = pos_at(f, e->x);
        f->sel = -1;
        widget_capture(w);
        widget_invalidate(w);
        return 1;
    case EV_MOUSE_MOVE:
        if (e->button & 1) {
            int p = pos_at(f, e->x);
            if (p != f->cursor) {
                if (f->sel < 0)
                    f->sel = f->cursor;
                f->cursor = p;
                scroll_to_cursor(f, widget_theme(w));
                widget_invalidate(w);
            }
            return 1;
        }
        return 0;
    case EV_KEY_DOWN:
        clamp(f);
        return key(f, e);
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

const struct widget_class textfield_class = { "textfield", sizeof(struct textfield), textfield_measure, NULL, textfield_paint, textfield_event, NULL };

struct widget *textfield_new(struct widget *parent, const char *text)
{
    struct widget *w = widget_new(&textfield_class, parent);
    if (!w)
        return NULL;
    struct textfield *f = (struct textfield *)w;
    w->focusable = 1;
    widget_set_text(w, text);
    f->cursor = len_of(f);
    f->sel = -1;
    widget_set_stretch(w, 1, 0);
    return w;
}
