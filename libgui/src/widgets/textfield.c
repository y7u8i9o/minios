/* Single line text field with cursor, selection and the clipboard. */
#include <gui/app.h>
#include <gui/utf8.h>
#include <stdlib.h>
#include <string.h>
#include "editmenu.h"

#define PAD 4

struct textfield {
    struct widget w;
    int cursor, sel;            /* sel: anchor, -1 none */
    int scroll_x;
    int masked;                 /* shows one '*' per byte and never copies */
    char *mask;                 /* the stars, as long as the text */
    char preedit[WSRV_TITLE_MAX];
    struct widget *context_menu;
};

static void changed(struct textfield *f)
{
    struct sig_change c = { 0, widget_text(&f->w) };
    widget_invalidate(&f->w);
    widget_emit(&f->w, "changed", &c);
}

static int len_of(struct textfield *f) { return (int)strlen(widget_text(&f->w)); }

/* The text as it is drawn and measured: the text itself, or for a masked
 * field one '*' per byte, which keeps byte offsets valid as positions. */
static const char *shown(struct textfield *f)
{
    if (!f->masked)
        return widget_text(&f->w);
    int n = len_of(f);
    char *m = realloc(f->mask, (size_t)n + 1);
    if (!m)
        return "";
    f->mask = m;
    memset(m, '*', (size_t)n);
    m[n] = '\0';
    return m;
}

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
    int cx = gfx_text_width_font(t->font, shown(f), f->cursor);
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
    const char *text = shown(f);
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
        widget_text_cursor(w, 1 + cx, ty - 1, 1, th + 2);
        if (f->preedit[0]) {
            painter_text(p, cx, ty, f->preedit, t->color[TC_TEXT]);
            int pw = painter_text_width(p, f->preedit, -1);
            painter_line(p, cx, ty + th, cx + pw, ty + th, t->color[TC_ACCENT]);
            cx += pw;
        }
        painter_line(p, cx, ty - 1, cx, ty + th, t->color[TC_TEXT]);
    }
    painter_pop(p);
}

static int pos_at(struct textfield *f, int px)
{
    const struct theme *t = widget_theme(&f->w);
    int rel = px - PAD + f->scroll_x;
    return gfx_text_index_font(t->font, shown(f), -1, rel < 0 ? 0 : rel);
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
        if (f->sel >= 0 && f->sel != f->cursor && !f->masked) {
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
    case KEY_LEFT: f->cursor = gui_utf8_prev_boundary(widget_text(&f->w), f->cursor); break;
    case KEY_RIGHT: f->cursor = gui_utf8_next_boundary(widget_text(&f->w), len, f->cursor); break;
    case KEY_HOME: f->cursor = 0; break;
    case KEY_END: f->cursor = len; break;
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
    if (e->code == KEY_DELETE) {
        if (!delete_selection(f) && f->cursor < len) {
            int next = gui_utf8_next_boundary(f->w.text, len, f->cursor);
            memmove(f->w.text + f->cursor, f->w.text + next, (size_t)(len - next + 1));
        }
    } else if (e->ch == '\b') {
        if (!delete_selection(f) && f->cursor > 0) {
            int prev = gui_utf8_prev_boundary(f->w.text, f->cursor);
            memmove(f->w.text + prev, f->w.text + f->cursor, (size_t)(len - f->cursor + 1));
            f->cursor = prev;
        }
    } else if (e->ch == '\n') {
        struct sig_change c = { 0, widget_text(&f->w) };
        widget_emit(&f->w, "activate", &c);
        return 1;
    } else if (e->ch >= 32 && !ctrl) {
        char ch[4];
        int n = gui_utf8_encode((uint32_t)e->ch, ch);
        delete_selection(f);
        insert(f, ch, n);
    } else {
        return 0;
    }
    changed(f);
    scroll_to_cursor(f, t);
    return 1;
}

static void context_action(struct widget *w, enum edit_action a)
{
    struct textfield *f = (struct textfield *)w;
    int a0, b0;
    clamp(f);
    switch (a) {
    case EDIT_CUT:
    case EDIT_COPY:
        if (f->sel >= 0 && f->sel != f->cursor && !f->masked) {
            sel_range(f, &a0, &b0);
            gui_clipboard_set(widget_text(w) + a0, b0 - a0);
            if (a == EDIT_CUT && delete_selection(f))
                changed(f);
        }
        break;
    case EDIT_PASTE: {
        struct event e = { .type = EV_KEY_DOWN, .code = KEY_V, .ch = 22, .mods = WMOD_CTRL };
        key(f, &e);
        break;
    }
    case EDIT_DELETE:
        if (delete_selection(f))
            changed(f);
        break;
    case EDIT_SELECT_ALL:
        f->sel = 0;
        f->cursor = len_of(f);
        break;
    default:
        break;
    }
    widget_focus(w);
    widget_invalidate(w);
}

static void context_menu(struct textfield *f, int x)
{
    int p = pos_at(f, x), a, b;
    int sel = f->sel >= 0 && f->sel != f->cursor;
    if (sel) {
        sel_range(f, &a, &b);
        sel = p >= a && p <= b;
    }
    if (!sel) {
        f->cursor = p;
        f->sel = -1;
    }
    widget_focus(&f->w);
    widget_invalidate(&f->w);
    unsigned enabled = EDIT_BIT(EDIT_PASTE) | EDIT_BIT(EDIT_SELECT_ALL);
    if (sel)
        enabled |= (f->masked ? 0 : EDIT_BIT(EDIT_CUT) | EDIT_BIT(EDIT_COPY)) | EDIT_BIT(EDIT_DELETE);
    unsigned shown = EDIT_BIT(EDIT_CUT) | EDIT_BIT(EDIT_COPY) | EDIT_BIT(EDIT_PASTE) | EDIT_BIT(EDIT_DELETE) |
                     EDIT_BIT(EDIT_SELECT_ALL);
    edit_menu_popup(&f->w, &f->context_menu, x, f->w.h / 2, shown, enabled, context_action);
}

static int textfield_event(struct widget *w, struct event *e)
{
    struct textfield *f = (struct textfield *)w;
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (e->button & 2) {
            clamp(f);
            context_menu(f, e->x);
            return 1;
        }
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
    case EV_TEXT:
        if (e->text && *e->text) {
            f->preedit[0] = '\0';
            int n = (int)strlen(e->text);
            char *copy = malloc((size_t)n);
            if (!copy)
                return 1;
            for (int i = 0; i < n; i++)
                copy[i] = e->text[i] == '\n' || e->text[i] == '\r' ? ' ' : e->text[i];
            delete_selection(f);
            insert(f, copy, n);
            free(copy);
            changed(f);
            scroll_to_cursor(f, widget_theme(w));
        }
        return 1;
    case EV_PREEDIT:
        strlcpy(f->preedit, e->text ? e->text : "", sizeof f->preedit);
        widget_invalidate(w);
        return 1;
    case EV_TEXT_DELETE:
        if (e->before >= 0 && e->after >= 0) {
            int len = len_of(f);
            int a = f->cursor - e->before, b = f->cursor + e->after;
            if (a < 0) a = 0;
            if (b > len) b = len;
            while (a > 0 && ((unsigned char)f->w.text[a] & 0xc0) == 0x80) a--;
            while (b < len && ((unsigned char)f->w.text[b] & 0xc0) == 0x80) b++;
            memmove(f->w.text + a, f->w.text + b, (size_t)(len - b + 1));
            f->cursor = a;
            changed(f);
        }
        return 1;
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

static void textfield_destroy(struct widget *w)
{
    struct textfield *f = (struct textfield *)w;
    if (f->mask) {
        memset(f->mask, 0, strlen(f->mask));
        free(f->mask);
    }
}

const struct widget_class textfield_class = { "textfield", sizeof(struct textfield), textfield_measure, NULL, textfield_paint, textfield_event, textfield_destroy };

void textfield_set_masked(struct widget *w, int masked)
{
    struct textfield *f = (struct textfield *)w;
    f->masked = masked != 0;
    f->scroll_x = 0;
    widget_invalidate(w);
}

struct widget *textfield_new(struct widget *parent, const char *text)
{
    struct widget *w = widget_new(&textfield_class, parent);
    if (!w)
        return NULL;
    struct textfield *f = (struct textfield *)w;
    w->focusable = 1;
    w->accepts_text = 1;
    widget_set_text(w, text);
    f->cursor = len_of(f);
    f->sel = -1;
    widget_set_stretch(w, 1, 0);
    return w;
}
