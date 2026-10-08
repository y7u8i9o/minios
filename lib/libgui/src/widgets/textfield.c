/* Single line text field with cursor, selection, the clipboard and
 * dragging of the selection. */
#include <gui/app.h>
#include <gui/utf8.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "editmenu.h"

/* The space between the frame and the text. */
static int pad(const struct widget *w)
{
    return theme_scale_px(widget_theme(w), 4);
}

struct textfield {
    struct widget w;
    int cursor, sel;            /* sel: anchor, -1 none */
    int scroll_x;
    int masked;                 /* shows one '*' per byte and never copies */
    char *mask;                 /* the stars, as long as the text */
    char preedit[WSRV_TITLE_MAX];
    struct widget *context_menu;
    /* A press inside the selection that may become a drag, and the
     * dragged bytes and text while the drag runs. */
    int drag_pending, press_x, press_y;
    int drag_out, drag_a, drag_b;
    char *drag_text;
    /* The undo and redo history: the text and the cursor before each
     * step. merge allows the last step to absorb typed characters. A
     * masked field records no history. */
    struct snapshot { char *text; int cursor; } *undo, *redo;
    int nundo, nredo, merge;
    char *placeholder;          /* shown in the disabled text colour while the text is empty */
    struct gui_clicks clicks;
};

#define HISTORY_MAX 100

static void push_snapshot(struct snapshot **stack, int *n, const char *text, int cursor)
{
    if (*n == HISTORY_MAX) {
        free((*stack)[0].text);
        memmove(*stack, *stack + 1, (size_t)(HISTORY_MAX - 1) * sizeof **stack);
        (*n)--;
    }
    struct snapshot *grown = realloc(*stack, (size_t)(*n + 1) * sizeof *grown);
    char *copy = strdup(text);
    if (!grown || !copy) {
        free(copy);
        if (grown)
            *stack = grown;
        return;
    }
    *stack = grown;
    grown[(*n)++] = (struct snapshot){ copy, cursor };
}

static void clear_snapshots(struct snapshot **stack, int *n)
{
    for (int i = 0; i < *n; i++)
        free((*stack)[i].text);
    free(*stack);
    *stack = NULL;
    *n = 0;
}

static void changed(struct textfield *f)
{
    struct sig_change c = { 0, widget_text(&f->w) };
    widget_invalidate(&f->w);
    widget_emit(&f->w, "changed", &c);
}

static const char *shown(struct textfield *f);

/* The x of the caret before the byte offset at, in the coordinates of the
 * field. The text starts at the padding minus scroll_x. */
static int caret_x(struct textfield *f, int at)
{
    return pad(&f->w) - f->scroll_x + widget_text_width(&f->w, NULL, shown(f), at);
}

/* The state before an edit or a caret move, for the partial repaint. */
struct edit_start {
    int x, scroll_x, sel;
};

static struct edit_start before_edit(struct textfield *f, int at)
{
    return (struct edit_start){ caret_x(f, at), f->scroll_x, f->sel >= 0 && f->sel != f->cursor };
}

/* An edit changed the text from the offset of b.x on. The field repaints
 * from that column to its end. A glyph may reach 2 pixels to the left of
 * its position. A changed scroll offset or a selection repaints the whole
 * field. */
static void edited(struct textfield *f, struct edit_start b)
{
    struct sig_change c = { 0, widget_text(&f->w) };
    if (f->scroll_x != b.scroll_x || b.sel || f->preedit[0])
        widget_invalidate(&f->w);
    else
        widget_invalidate_rect(&f->w, (struct rect){ b.x - 2, 0, f->w.w - b.x + 2, f->w.h });
    widget_emit(&f->w, "changed", &c);
}

/* A caret move repaints the old and the new caret column. */
static void caret_moved(struct textfield *f, struct edit_start b)
{
    if (f->scroll_x != b.scroll_x || b.sel || (f->sel >= 0 && f->sel != f->cursor)) {
        widget_invalidate(&f->w);
        return;
    }
    int x = caret_x(f, f->cursor);
    widget_invalidate_rect(&f->w, (struct rect){ b.x - 1, 0, 3, f->w.h });
    widget_invalidate_rect(&f->w, (struct rect){ x - 1, 0, 3, f->w.h });
}

static int len_of(struct textfield *f) { return (int)strlen(widget_text(&f->w)); }

/* The text as it is drawn and measured: the text itself, or for a masked
 * field one '*' per byte, which preserves the validity of byte offsets as positions. */
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

/* widget_set_text replaces the text without the knowledge of the field.
 * The paint and event functions therefore clamp the cursor and the
 * anchor to the length of the current text before they use them. */
static void clamp(struct textfield *f)
{
    int n = len_of(f);
    if (f->cursor > n) f->cursor = n;
    if (f->cursor < 0) f->cursor = 0;
    if (f->sel > n) f->sel = n;
}

/* Records the text before an edit. Typed characters merge into one step
 * until the cursor moves or another kind of edit happens. */
static void remember(struct textfield *f, int mergeable)
{
    if (f->masked)
        return;
    if (!(mergeable && f->merge && f->nundo))
        push_snapshot(&f->undo, &f->nundo, widget_text(&f->w), f->cursor);
    clear_snapshots(&f->redo, &f->nredo);
    f->merge = mergeable;
}

/* Undo (redo 0) or redo (redo 1) one step. Returns 1 when a step existed. */
static int restore(struct textfield *f, int redo)
{
    struct snapshot **from = redo ? &f->redo : &f->undo, **to = redo ? &f->undo : &f->redo;
    int *nfrom = redo ? &f->nredo : &f->nundo, *nto = redo ? &f->nundo : &f->nredo;
    if (!*nfrom)
        return 0;
    push_snapshot(to, nto, widget_text(&f->w), f->cursor);
    struct snapshot s = (*from)[--*nfrom];
    free(f->w.text);
    f->w.text = s.text;
    f->cursor = s.cursor;
    f->sel = -1;
    f->merge = 0;
    changed(f);
    return 1;
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

/* The scroll offset shows the cursor. The offset never leaves free space
 * after the end of the text. A shorter text therefore scrolls back. */
static void scroll_to_cursor(struct textfield *f)
{
    const char *text = shown(f);
    int cx = widget_text_width(&f->w, NULL, text, f->cursor);
    int avail = f->w.w - 2 * pad(&f->w), end = widget_text_width(&f->w, NULL, text, -1) - avail + 1;
    if (cx < f->scroll_x) f->scroll_x = cx;
    if (cx > f->scroll_x + avail - 1) f->scroll_x = cx - avail + 1;
    if (f->scroll_x > end) f->scroll_x = end;
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
    clamp(f);
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    painter_field(p, 0, 0, w->w, w->h, widget_paint_state(w) & (PAINTER_DISABLED | PAINTER_FOCUSED));
    painter_push(p, 1, 1, w->w - 2, w->h - 2);
    const char *text = shown(f);
    int th = painter_text_height(p);
    int ty = (w->h - 2 - th) / 2, tx = pad(&f->w) - 1 - f->scroll_x;
    if (f->sel >= 0 && f->sel != f->cursor) {
        int a, b;
        sel_range(f, &a, &b);
        int x0 = painter_text_width(p, text, a), x1 = painter_text_width(p, text, b);
        painter_fill(p, tx + x0, ty - 1, x1 - x0, th + 2, t->color[TC_SELECTION]);
    }
    if (!text[0] && f->placeholder && !f->preedit[0])
        painter_text(p, tx, ty, f->placeholder, t->color[TC_TEXT_DISABLED]);
    painter_text(p, tx, ty, text, t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED]);
    if (f->sel >= 0 && f->sel != f->cursor) {
        /* The selected text again, in the selection text colour. */
        int a, b;
        sel_range(f, &a, &b);
        int x0 = painter_text_width(p, text, a), x1 = painter_text_width(p, text, b);
        painter_push_clip(p, tx + x0, ty - 1, x1 - x0, th + 2);
        painter_text(p, tx, ty, text, t->color[TC_SELECTION_TEXT]);
        painter_pop(p);
    }
    if (w->focused) {
        int cx = tx + painter_text_width(p, text, f->cursor);
        widget_text_cursor(w, 1 + cx, ty - 1, 1, th + 2);
        if (f->preedit[0]) {
            painter_text(p, cx, ty, f->preedit, t->color[TC_TEXT]);
            int pw = painter_text_width(p, f->preedit, -1);
            painter_line(p, cx, ty + th, cx + pw, ty + th, t->color[TC_ACCENT]);
            cx += pw;
        }
        if (widget_caret_visible(w))
            painter_line(p, cx, ty - 1, cx, ty + th, t->color[TC_TEXT]);
    }
    painter_pop(p);
}

static int pos_at(struct textfield *f, int px)
{
    int rel = px - pad(&f->w) + f->scroll_x;
    return widget_text_index(&f->w, NULL, shown(f), -1, rel < 0 ? 0 : rel);
}

static int key(struct textfield *f, struct event *e)
{
    int ctrl = e->mods & WMOD_CTRL, shift = e->mods & WMOD_SHIFT;
    int before = f->cursor, before_sel = f->sel, before_scroll = f->scroll_x, len = len_of(f);
    struct edit_start b = before_edit(f, f->cursor);
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
                remember(f, 0);
                delete_selection(f);
                changed(f);
            }
        }
        return 1;
    }
    if (ctrl && (e->ch == 26 || e->ch == 25)) {  /* Ctrl+Z, Ctrl+Y */
        if (restore(f, e->ch == 25))
            scroll_to_cursor(f);
        return 1;
    }
    if (ctrl && e->ch == 22) {                   /* Ctrl+V */
        char *tmp = malloc(WSRV_CLIP_MAX);
        int n = tmp ? gui_clipboard_get(tmp, WSRV_CLIP_MAX) : -1;
        if (n > 0) {
            for (int i = 0; i < n; i++)
                if (tmp[i] == '\n')
                    tmp[i] = ' ';
            remember(f, 0);
            delete_selection(f);
            insert(f, tmp, n);
            changed(f);
        }
        free(tmp);
        scroll_to_cursor(f);
        return 1;
    }
    switch (e->code) {
    case KEY_LEFT:
        f->cursor = ctrl ? gui_word_left(widget_text(&f->w), f->cursor) : gui_utf8_prev_boundary(widget_text(&f->w), f->cursor);
        break;
    case KEY_RIGHT:
        f->cursor = ctrl ? gui_word_right(widget_text(&f->w), len, f->cursor)
                         : gui_utf8_next_boundary(widget_text(&f->w), len, f->cursor);
        break;
    case KEY_HOME: f->cursor = 0; break;
    case KEY_END: f->cursor = len; break;
    default: moved = 0;
    }
    if (moved) {
        f->merge = 0;
        if (shift) {
            if (f->sel < 0)
                f->sel = before;
        } else {
            f->sel = -1;
        }
        scroll_to_cursor(f);
        /* Home at the start and End at the end change nothing. */
        if (f->cursor != before || f->sel != before_sel || f->scroll_x != before_scroll)
            caret_moved(f, b);
        return 1;
    }
    if (e->code == KEY_DELETE) {
        remember(f, 0);
        if (!delete_selection(f) && f->cursor < len) {
            int next = gui_utf8_next_boundary(f->w.text, len, f->cursor);
            memmove(f->w.text + f->cursor, f->w.text + next, (size_t)(len - next + 1));
        }
    } else if (e->ch == '\b') {
        if (f->cursor > 0 && !b.sel)
            b = before_edit(f, gui_utf8_prev_boundary(f->w.text, f->cursor));
        remember(f, 0);
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
        remember(f, !b.sel);
        delete_selection(f);
        insert(f, ch, n);
    } else {
        return 0;
    }
    scroll_to_cursor(f);
    edited(f, b);
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
            if (a == EDIT_CUT) {
                remember(f, 0);
                if (delete_selection(f))
                    changed(f);
            }
        }
        break;
    case EDIT_UNDO:
    case EDIT_REDO:
        restore(f, a == EDIT_REDO);
        break;
    case EDIT_PASTE: {
        struct event e = { .type = EV_KEY_DOWN, .code = KEY_V, .ch = 22, .mods = WMOD_CTRL };
        key(f, &e);
        break;
    }
    case EDIT_DELETE:
        remember(f, 0);
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
    if (f->nundo)
        enabled |= EDIT_BIT(EDIT_UNDO);
    if (f->nredo)
        enabled |= EDIT_BIT(EDIT_REDO);
    unsigned shown = EDIT_BIT(EDIT_CUT) | EDIT_BIT(EDIT_COPY) | EDIT_BIT(EDIT_PASTE) | EDIT_BIT(EDIT_DELETE) |
                     EDIT_BIT(EDIT_SELECT_ALL);
    if (!f->masked)
        shown |= EDIT_BIT(EDIT_UNDO) | EDIT_BIT(EDIT_REDO);
    edit_menu_popup(&f->w, &f->context_menu, x, f->w.h / 2, shown, enabled, context_action);
}

/* ---- dragging the selection ---- */

static void drag_begin(struct textfield *f)
{
    int a, b;
    sel_range(f, &a, &b);
    char *text = strndup(widget_text(&f->w) + a, (size_t)(b - a));
    if (!text)
        return;
    char label[48];
    snprintf(label, sizeof label, b - a > 32 ? "%.32s…" : "%s", text);
    struct gui_drag_item item = { "text/plain", text, (size_t)(b - a) };
    if (widget_drag_start(&f->w, &item, 1, GUI_DND_COPY | GUI_DND_MOVE, NULL, label) == 0) {
        f->drag_out = 1;
        f->drag_a = a;
        f->drag_b = b;
        free(f->drag_text);
        f->drag_text = text;
    } else {
        free(text);
    }
}

/* Text moved elsewhere is removed here, when the field still contains it. */
static void drag_end(struct textfield *f, int action)
{
    const char *t = widget_text(&f->w);
    if (f->drag_out && action == GUI_DND_MOVE && f->drag_b <= len_of(f) &&
        strncmp(t + f->drag_a, f->drag_text, (size_t)(f->drag_b - f->drag_a)) == 0) {
        remember(f, 0);
        f->sel = f->drag_a;
        f->cursor = f->drag_b;
        delete_selection(f);
        changed(f);
    }
    f->drag_out = 0;
    free(f->drag_text);
    f->drag_text = NULL;
}

static int textfield_event(struct widget *w, struct event *e)
{
    struct textfield *f = (struct textfield *)w;
    clamp(f);
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (e->button & 2) {
            clamp(f);
            context_menu(f, e->x);
            return 1;
        }
        if (!(e->button & 1))
            return 0;
        {
            /* A double click selects a word, a triple click the text. */
            int clicks = gui_click_count(&f->clicks, e->x, e->y), p = pos_at(f, e->x), a, b;
            if (clicks > 1) {
                f->merge = 0;
                if (clicks == 2 && !f->masked)
                    gui_word_at(widget_text(w), len_of(f), p, &a, &b);
                else
                    a = 0, b = len_of(f);
                f->sel = a;
                f->cursor = b;
                scroll_to_cursor(f);
                widget_invalidate(w);
                return 1;
            }
            /* A press inside the selection of a field that may copy may
             * drag it; a release without a move clears the selection. */
            sel_range(f, &a, &b);
            if (!f->masked && f->sel >= 0 && a < b && p >= a && p <= b) {
                f->drag_pending = 1;
                f->press_x = e->x;
                f->press_y = e->y;
                widget_capture(w);
                return 1;
            }
            f->cursor = p;
        }
        f->sel = -1;
        f->merge = 0;
        widget_capture(w);
        widget_invalidate(w);
        return 1;
    case EV_MOUSE_UP:
        if (f->drag_pending) {
            f->drag_pending = 0;
            f->cursor = pos_at(f, f->press_x);
            f->sel = -1;
            widget_invalidate(w);
            return 1;
        }
        return 0;
    case EV_DRAG_END:
        drag_end(f, e->drag->action);
        return 1;
    case EV_MOUSE_MOVE:
        if (f->drag_pending) {
            if ((e->button & 1) && widget_drag_moved(f->press_x, f->press_y, e->x, e->y)) {
                f->drag_pending = 0;
                drag_begin(f);
            }
            return 1;
        }
        if (e->button & 1) {
            int p = pos_at(f, e->x);
            if (p != f->cursor) {
                if (f->sel < 0)
                    f->sel = f->cursor;
                f->cursor = p;
                scroll_to_cursor(f);
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
            struct edit_start b = before_edit(f, f->cursor);
            remember(f, !b.sel && n == 1);
            delete_selection(f);
            insert(f, copy, n);
            free(copy);
            scroll_to_cursor(f);
            edited(f, b);
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
            remember(f, 0);
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
    free(f->drag_text);
    free(f->placeholder);
    clear_snapshots(&f->undo, &f->nundo);
    clear_snapshots(&f->redo, &f->nredo);
    if (f->mask) {
        memset(f->mask, 0, strlen(f->mask));
        free(f->mask);
    }
}

const struct widget_class textfield_class = { "textfield", sizeof(struct textfield), textfield_measure, NULL, textfield_paint, textfield_event, textfield_destroy };

void textfield_set_placeholder(struct widget *w, const char *text)
{
    struct textfield *f = (struct textfield *)w;
    free(f->placeholder);
    f->placeholder = text && *text ? strdup(text) : NULL;
    widget_invalidate(w);
}

void textfield_set_masked(struct widget *w, int masked)
{
    struct textfield *f = (struct textfield *)w;
    f->masked = masked != 0;
    clear_snapshots(&f->undo, &f->nundo);
    clear_snapshots(&f->redo, &f->nredo);
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

void textfield_select(struct widget *w, int anchor, int cursor)
{
    struct textfield *f = (struct textfield *)w;
    int n = len_of(f);
    f->cursor = cursor < 0 || cursor > n ? n : cursor;
    f->sel = anchor < 0 ? -1 : anchor > n ? n : anchor;
    scroll_to_cursor(f);
    widget_invalidate(w);
}
