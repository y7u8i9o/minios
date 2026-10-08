/* Tree view and table over struct model: virtualised rows, expansion,
 * selection, sortable and resizable columns. */
#include <gui/app.h>
#include <gui/model.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../intmap.h"


struct view {
    struct widget w;
    struct model *m;
    int scroll;                 /* first visible flat row */
    /* tree: the set of expanded row ids, the flattened visible rows and
     * the map from a row id to its flat row */
    struct intmap expanded, flat_of;
    int *flat, *depth, nflat;
    /* table: column widths, sort state, header drag */
    int *widths, ncols;
    int sort_col, sort_desc;
    int drag_col, drag_x0, drag_w0;
    int header;                 /* 1 for tables */
    struct gui_clicks clicks;   /* the left clicks, for double clicks */
    int click_row;              /* the flat row of the last left click */
    struct scroll_track track;
    int hot;                    /* the flat row under the pointer, or -1 */
    int hot_col, pressed_col;   /* the header cell under the pointer and the pressed one, or -1 */
    int press_row, press_x, press_y;    /* a row press that may become a drag */
    int drop_row;               /* outlined drop target: a row, -1 the view, -2 none */
};


static int line_h(const struct widget *w)
{
    const struct theme *t = widget_theme(w);
    return t->font->height + theme_px(t, TM_ROW_PAD);
}

/* The indentation of a tree level and the width of an icon column. */
static int indent(const struct widget *w) { return theme_px(widget_theme(w), TM_INDENT); }
static int icon_w(const struct widget *w) { return theme_px(widget_theme(w), TM_ICON) + 4; }

/* The height of the header row of a table, 0 for a tree view. */
static int header_h(const struct view *v)
{
    return v->header ? theme_scale_px(widget_theme(&v->w), 24) : 0;
}

static int is_expanded(struct view *v, int row)
{
    return intmap_get(&v->expanded, row, 0);
}

static void flatten(struct view *v, int parent, int depth)
{
    if (!v->m)
        return;
    int n = v->m->rows(v->m, parent);
    for (int i = 0; i < n; i++) {
        int row = v->m->child(v->m, parent, i);
        if ((v->nflat & 63) == 0) {
            v->flat = realloc(v->flat, (size_t)(v->nflat + 64) * sizeof *v->flat);
            v->depth = realloc(v->depth, (size_t)(v->nflat + 64) * sizeof *v->depth);
        }
        v->flat[v->nflat] = row;
        v->depth[v->nflat] = depth;
        intmap_put(&v->flat_of, row, v->nflat);
        v->nflat++;
        if (!v->header && is_expanded(v, row))
            flatten(v, row, depth + 1);
    }
}

static void refresh(struct view *v)
{
    v->nflat = 0;
    v->hot = -1;
    intmap_clear(&v->flat_of);
    flatten(v, -1, 0);
    if (v->header && v->m) {
        int nc = v->m->columns(v->m);
        if (nc != v->ncols) {
            v->widths = realloc(v->widths, (size_t)nc * sizeof *v->widths);
            for (int i = v->ncols; i < nc; i++)
                v->widths[i] = 100;
            v->ncols = nc;
        }
    }
    widget_invalidate(&v->w);
}

static int rows_visible(struct view *v)
{
    int lh = line_h(&v->w);
    return (v->w.h - 2 - (header_h(v))) / lh;
}

static int flat_index_of(struct view *v, int row)
{
    return intmap_get(&v->flat_of, row, -1);
}

/* The rectangle of the rows below the header and left of the track. */
static struct rect rows_rect(struct view *v)
{
    int top = header_h(v), rows = rows_visible(v);
    int sbw = v->nflat > rows ? theme_px(widget_theme(&v->w), TM_SCROLLBAR) : 0;
    return (struct rect){ 1, 1 + top, v->w.w - 2 - sbw, v->w.h - 2 - top };
}

/* The view scrolled from the first row old: the rows move by copy, and
 * the track is repainted. A move up exposes the rows at the bottom and
 * the partly shown row above them. */
static void scrolled(struct view *v, int old)
{
    struct widget *w = &v->w;
    if (v->drop_row != -2) {
        widget_invalidate(w);
        return;
    }
    struct rect r = rows_rect(v);
    int lh = line_h(w), dy = (old - v->scroll) * lh, rem = r.h % lh;
    widget_scroll_area(w, r, dy);
    if (dy < 0 && rem)
        widget_invalidate_rect(w, (struct rect){ r.x, r.y + r.h + dy - rem, r.w, rem });
    widget_invalidate_rect(w, (struct rect){ r.x + r.w, r.y, w->w - r.x - r.w, r.h });
}

static void select_flat(struct view *v, int idx, const char *signal)
{
    if (idx < 0 || idx >= v->nflat)
        return;
    int old_value = v->w.value, old_scroll = v->scroll;
    v->w.value = v->flat[idx];
    int rows = rows_visible(v);
    if (idx < v->scroll) v->scroll = idx;
    if (rows > 0 && idx >= v->scroll + rows) v->scroll = idx - rows + 1;
    if (v->w.value != old_value || v->scroll != old_scroll)
        widget_invalidate(&v->w);
    struct sig_select s = { v->w.value };
    widget_emit(&v->w, signal, &s);
}

static void view_measure(struct widget *w, struct size_hint *h)
{
    h->pref_w = 200;
    h->pref_h = 8 * line_h(w) + 2;
    h->min_w = 40;
    h->min_h = line_h(w) + 2;
}

/* An icon of a row. A selected row shows the icon in the selection text
 * colour. */
static void row_icon(struct painter *p, int x, int y, const struct image *icon, int selected, uint32_t fg)
{
    if (selected)
        painter_image(p, x, y, icon_variant(icon, p->scale, fg));
    else
        painter_icon(p, x, y, icon, 0);
}

/* Repaints the flat row idx where it is visible. */
static void invalidate_row(struct view *v, int idx)
{
    struct rect r = rows_rect(v);
    int lh = line_h(&v->w), i = idx - v->scroll;
    if (idx >= 0 && i >= 0 && i <= rows_visible(v))
        widget_invalidate_rect(&v->w, (struct rect){ r.x, r.y + i * lh, r.w, lh });
}

static void set_hot(struct view *v, int idx)
{
    if (idx == v->hot)
        return;
    invalidate_row(v, v->hot);
    v->hot = idx;
    invalidate_row(v, idx);
}

/* The header cell at a local position, or -1. */
static int header_col_at(struct view *v, int x, int y)
{
    if (!v->header || y < 1 || y >= 1 + header_h(v))
        return -1;
    int cx = 1;
    for (int c = 0; c < v->ncols; c++) {
        if (x >= cx && x < cx + v->widths[c])
            return c;
        cx += v->widths[c];
    }
    return -1;
}

static void set_header_state(struct view *v, int hot, int pressed)
{
    if (hot == v->hot_col && pressed == v->pressed_col)
        return;
    v->hot_col = hot;
    v->pressed_col = pressed;
    widget_invalidate_rect(&v->w, (struct rect){ 1, 1, v->w.w - 2, header_h(v) });
}

static void view_paint(struct widget *w, struct painter *p)
{
    struct view *v = (struct view *)w;
    const struct theme *t = p->theme;
    int lh = line_h(w), rows = rows_visible(v);
    int sbw = v->nflat > rows ? theme_px(t, TM_SCROLLBAR) : 0;
    int top = header_h(v);
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    char buf[256];
    if (v->header) {
        painter_push(p, 1, 1, w->w - 2 - sbw, header_h(v));
        painter_fill(p, 0, 0, w->w, header_h(v), t->color[TC_WINDOW]);
        int x = 0;
        for (int c = 0; c < v->ncols; c++) {
            const char *hd = v->m && v->m->header ? v->m->header(v->m, c) : "";
            painter_push(p, x, 0, v->widths[c], header_h(v));
            if (w->enabled && (c == v->pressed_col || c == v->hot_col))
                painter_fill(p, 0, 0, v->widths[c] - 1, header_h(v) - 1,
                             t->color[c == v->pressed_col ? TC_BUTTON_PRESSED : TC_BUTTON_HOVER]);
            uint32_t hfg = t->color[w->enabled ? TC_TEXT : TC_TEXT_DISABLED];
            painter_text(p, 4, (header_h(v) - painter_text_height(p)) / 2, hd ? hd : "", hfg);
            if (c == v->sort_col) {
                int m = theme_px(t, TM_ICON) * 3 / 4;
                painter_chevron(p, v->widths[c] - m - 4, (header_h(v) - m) / 2, m,
                                v->sort_desc ? PAINTER_DOWN : PAINTER_UP, hfg);
            }
            painter_pop(p);
            x += v->widths[c];
            painter_line(p, x - 1, 0, x - 1, header_h(v) - 1, t->color[TC_BORDER]);
        }
        painter_line(p, 0, header_h(v) - 1, w->w, header_h(v) - 1, t->color[TC_BORDER]);
        painter_pop(p);
    }
    painter_push(p, 1, 1 + top, w->w - 2 - sbw, w->h - 2 - top);
    /* Only the rows inside the clip, for a partial paint. */
    struct rect clip = painter_clip_local(p);
    int i0 = clip.y > 0 ? clip.y / lh : 0, i1 = rect_empty(p->clip) ? 0 : (clip.y + clip.h + lh - 1) / lh;
    for (int i = i0; i <= rows && i < i1 && v->scroll + i < v->nflat; i++) {
        int idx = v->scroll + i, row = v->flat[idx];
        int y = i * lh;
        int selected = row == w->value;
        /* Selected and hovered rows are rounded pills inside the view. */
        if (selected || (idx == v->hot && w->enabled))
            painter_rounded(p, 1, y + 1, w->w - 4 - sbw, lh - 2,
                            t->color[selected ? (w->enabled ? TC_SELECTION : TC_TRACK) : TC_BUTTON_HOVER], PAINTER_NONE);
        uint32_t fg = t->color[!w->enabled ? TC_TEXT_DISABLED : selected ? TC_SELECTION_TEXT : TC_TEXT];
        int ty = y + (lh - painter_text_height(p)) / 2;
        const struct image *icon = v->m->icon ? v->m->icon(v->m, row) : NULL;
        if (v->header) {
            int x = 0;
            for (int c = 0; c < v->ncols; c++) {
                const char *s = v->m->cell(v->m, row, c, buf, sizeof buf);
                painter_push(p, x, y, v->widths[c] - 1, lh);
                int tx = 4;
                if (c == 0 && icon) {
                    row_icon(p, 4, (lh - image_lh(icon)) / 2, icon, selected, fg);
                    tx += icon_w(w);
                }
                painter_text(p, tx, ty - y, s ? s : "", fg);
                painter_pop(p);
                x += v->widths[c];
            }
        } else {
            int x = 4 + v->depth[idx] * indent(w);
            if (icon) {
                row_icon(p, x + indent(w), y + (lh - image_lh(icon)) / 2, icon, selected, fg);
                x += icon_w(w);
            }
            if (v->m->rows(v->m, row) > 0) {
                int e = indent(w);
                painter_chevron(p, x, y + (lh - e) / 2, e, is_expanded(v, row) ? PAINTER_DOWN : PAINTER_RIGHT, fg);
            }
            const char *s = v->m->cell(v->m, row, 0, buf, sizeof buf);
            painter_text(p, x + indent(w), ty, s ? s : "", fg);
        }
    }
    painter_pop(p);
    if (sbw)
        scrollbar_paint_track(p, w->w - sbw, top, sbw, w->h - top, v->scroll, v->nflat, rows, 1);
    if (v->drop_row == -1) {
        painter_frame(p, 0, 0, w->w, w->h, t->color[TC_ACCENT]);
        painter_frame(p, 1, 1, w->w - 2, w->h - 2, t->color[TC_ACCENT]);
    } else if (v->drop_row >= 0) {
        int idx = flat_index_of(v, v->drop_row);
        if (idx >= v->scroll && idx <= v->scroll + rows) {
            painter_push(p, 1, 1 + top, w->w - 2 - sbw, w->h - 2 - top);
            painter_frame(p, 0, (idx - v->scroll) * lh, w->w - 2 - sbw, lh, t->color[TC_ACCENT]);
            painter_frame(p, 1, (idx - v->scroll) * lh + 1, w->w - 4 - sbw, lh - 2, t->color[TC_ACCENT]);
            painter_pop(p);
        }
    }
}

/* The row id under a local position, or -1 below the rows and in the header. */
static int row_at_y(struct view *v, int y)
{
    int top = header_h(v);
    if (y < top + 1)
        return -1;
    int idx = v->scroll + (y - 1 - top) / line_h(&v->w);
    return idx >= 0 && idx < v->nflat ? v->flat[idx] : -1;
}

static void set_drop_row(struct view *v, int row)
{
    if (v->drop_row != row) {
        v->drop_row = row;
        widget_invalidate(&v->w);
    }
}

static int view_drag_event(struct widget *w, struct event *e)
{
    struct view *v = (struct view *)w;
    struct sig_drag sd = { row_at_y(v, e->y), e->x, e->y, e->drag };
    switch (e->type) {
    case EV_DRAG_MOVE:
        if (!widget_emit(w, "drag_motion", &sd))
            return 0;
        set_drop_row(v, e->drag->accept_mime ? sd.row : -2);
        return 1;
    case EV_DROP:
        sd.row = v->drop_row >= 0 ? v->drop_row : -1;
        widget_emit(w, "drop", &sd);
        return 1;
    case EV_DRAG_LEAVE:
        set_drop_row(v, -2);
        widget_emit(w, "drag_leave", &sd);
        return 1;
    case EV_DRAG_END:
        sd.row = -1;
        widget_emit(w, "drag_end", &sd);
        return 1;
    default:
        return 0;
    }
}

static void toggle_expand(struct view *v, int row)
{
    if (is_expanded(v, row))
        intmap_remove(&v->expanded, row);
    else
        intmap_put(&v->expanded, row, 1);
    refresh(v);
}

/* The scroll track at the right edge below the header. */
static int track_event(struct view *v, struct event *e)
{
    struct widget *w = &v->w;
    int rows = rows_visible(v), top = header_h(v);
    int sbw = v->nflat > rows ? theme_px(widget_theme(w), TM_SCROLLBAR) : 0;
    int before = v->scroll;
    struct rect track = { w->w - sbw, top, sbw, w->h - top };
    if (!sbw || !scroll_track_event(&v->track, w, e, track, &v->scroll, v->nflat, rows))
        return 0;
    if (v->scroll != before)
        scrolled(v, before);
    return 1;
}

static int view_event(struct widget *w, struct event *e)
{
    struct view *v = (struct view *)w;
    int lh = line_h(w), rows = rows_visible(v);
    int top = header_h(v);
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (e->button & 2) {
            int idx = v->scroll + (e->y - 1 - top) / lh;
            if (e->y >= top + 1 && idx >= 0 && idx < v->nflat)
                select_flat(v, idx, "selected");
            widget_focus(w);
            struct sig_click c = { e->button, e->x, e->y };
            widget_emit(w, "context", &c);
            return 1;
        }
        if (!(e->button & 1))
            return 0;
        if (track_event(v, e))
            return 1;
        if (v->header && e->y < header_h(v) + 1) {
            int x = 1;
            for (int c = 0; c < v->ncols; c++) {
                int right = x + v->widths[c];
                if (e->x >= right - 4 && e->x < right + 4) {
                    v->drag_col = c;
                    v->drag_x0 = e->x;
                    v->drag_w0 = v->widths[c];
                    widget_capture(w);
                    return 1;
                }
                if (e->x >= x && e->x < right) {
                    set_header_state(v, c, c);
                    widget_capture(w);
                    if (v->m && v->m->sort) {
                        v->sort_desc = c == v->sort_col ? !v->sort_desc : 0;
                        v->sort_col = c;
                        v->m->sort(v->m, c, v->sort_desc);
                        refresh(v);
                    }
                    return 1;
                }
                x = right;
            }
            return 1;
        }
        int idx = v->scroll + (e->y - 1 - top) / lh;
        if (idx < 0 || idx >= v->nflat)
            return 1;
        if (!v->header) {
            int ex = 4 + v->depth[idx] * indent(w);
            if (e->x - 1 >= ex && e->x - 1 < ex + indent(w) && v->m->rows(v->m, v->flat[idx]) > 0) {
                toggle_expand(v, v->flat[idx]);
                return 1;
            }
        }
        int again = gui_click_count(&v->clicks, e->x, e->y) == 2 && idx == v->click_row;
        v->click_row = idx;
        select_flat(v, idx, again ? "activate" : "selected");
        if (!again) {
            v->press_row = v->flat[idx];
            v->press_x = e->x;
            v->press_y = e->y;
            widget_capture(w);
        }
        return 1;
    }
    case EV_MOUSE_MOVE:
        if (track_event(v, e))
            return 1;
        if (!(e->button & 1)) {
            struct rect r = rows_rect(v);
            int idx = v->scroll + (e->y - r.y) / lh;
            set_hot(v, rect_contains(r, e->x, e->y) && idx < v->nflat ? idx : -1);
            set_header_state(v, header_col_at(v, e->x, e->y), v->pressed_col);
        }
        if (v->drag_col >= 0 && (e->button & 1)) {
            int nw = v->drag_w0 + e->x - v->drag_x0;
            v->widths[v->drag_col] = nw < 20 ? 20 : nw;
            widget_invalidate(w);
            return 1;
        }
        if (v->press_row >= 0 && (e->button & 1) && widget_drag_moved(v->press_x, v->press_y, e->x, e->y)) {
            struct sig_drag sd = { v->press_row, e->x, e->y, NULL };
            v->press_row = -1;
            widget_emit(w, "drag_begin", &sd);
            return 1;
        }
        return 0;
    case EV_MOUSE_UP:
        track_event(v, e);
        v->drag_col = -1;
        v->press_row = -1;
        set_header_state(v, v->hot_col, -1);
        return 1;
    case EV_LEAVE:
        set_hot(v, -1);
        set_header_state(v, -1, v->pressed_col);
        return 1;
    case EV_DRAG_MOVE: case EV_DROP: case EV_DRAG_LEAVE: case EV_DRAG_END:
        return view_drag_event(w, e);
    case EV_MOUSE_WHEEL: {
        int old = v->scroll;
        if (scroll_set(&v->scroll, v->scroll + 3 * e->button, v->nflat, rows))
            scrolled(v, old);
        return 1;
    }
    case EV_KEY_DOWN: {
        if (e->mods & WMOD_ALT)
            return 0;                   /* accelerators such as Alt+Up */
        int idx = flat_index_of(v, w->value);
        switch (e->code) {
        case KEY_UP: select_flat(v, idx < 0 ? 0 : idx - 1, "selected"); return 1;
        case KEY_DOWN: select_flat(v, idx + 1, "selected"); return 1;
        case KEY_PAGEUP: select_flat(v, idx - rows < 0 ? 0 : idx - rows, "selected"); return 1;
        case KEY_PAGEDOWN: select_flat(v, idx + rows >= v->nflat ? v->nflat - 1 : idx + rows, "selected"); return 1;
        case KEY_HOME: select_flat(v, 0, "selected"); return 1;
        case KEY_END: select_flat(v, v->nflat - 1, "selected"); return 1;
        case KEY_RIGHT:
            if (!v->header && idx >= 0 && !is_expanded(v, w->value) && v->m->rows(v->m, w->value) > 0)
                toggle_expand(v, w->value);
            return 1;
        case KEY_LEFT:
            if (!v->header && idx >= 0 && is_expanded(v, w->value))
                toggle_expand(v, w->value);
            return 1;
        }
        if (e->ch == '\n' && idx >= 0) {
            select_flat(v, idx, "activate");
            return 1;
        }
        return 0;
    }
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

static void view_destroy(struct widget *w)
{
    struct view *v = (struct view *)w;
    intmap_free(&v->expanded);
    intmap_free(&v->flat_of);
    free(v->flat);
    free(v->depth);
    free(v->widths);
}

const struct widget_class treeview_class = { "treeview", sizeof(struct view), view_measure, NULL, view_paint, view_event, view_destroy };
const struct widget_class table_class = { "table", sizeof(struct view), view_measure, NULL, view_paint, view_event, view_destroy };

static struct widget *view_new(const struct widget_class *cls, struct widget *parent, int header)
{
    struct widget *w = widget_new(cls, parent);
    if (!w)
        return NULL;
    struct view *v = (struct view *)w;
    v->header = header;
    v->drag_col = -1;
    v->sort_col = -1;
    v->click_row = -1;
    v->track.grab = -1;
    v->hot = v->hot_col = v->pressed_col = -1;
    v->press_row = -1;
    v->drop_row = -2;
    w->value = -1;
    w->focusable = 1;
    widget_set_stretch(w, 1, 1);
    return w;
}

struct widget *treeview_new(struct widget *parent) { return view_new(&treeview_class, parent, 0); }
struct widget *table_new(struct widget *parent) { return view_new(&table_class, parent, 1); }

void view_set_model(struct widget *w, struct model *m)
{
    struct view *v = (struct view *)w;
    v->m = m;
    intmap_clear(&v->expanded);
    v->scroll = 0;
    w->value = -1;
    refresh(v);
}

void view_refresh(struct widget *w)
{
    struct view *v = (struct view *)w;
    refresh(v);
    v->scroll = scroll_clamp(v->scroll, v->nflat, rows_visible(v));
}

void treeview_expand(struct widget *w, int row, int expanded)
{
    struct view *v = (struct view *)w;
    if (is_expanded(v, row) != !!expanded)
        toggle_expand(v, row);
}

int treeview_is_expanded(const struct widget *w, int row)
{
    return is_expanded((struct view *)w, row);
}

int view_visible_rows(const struct widget *w)
{
    return ((const struct view *)w)->nflat;
}

int view_row_at(const struct widget *w, int index)
{
    const struct view *v = (const struct view *)w;
    return index >= 0 && index < v->nflat ? v->flat[index] : -1;
}

void view_select(struct widget *w, int row)
{
    struct view *v = (struct view *)w;
    int idx = flat_index_of(v, row);
    if (idx >= 0)
        select_flat(v, idx, "selected");
}

int view_row_rect(struct widget *w, int row, struct rect *r)
{
    struct view *v = (struct view *)w;
    int idx = flat_index_of(v, row);
    if (idx < v->scroll || idx >= v->scroll + rows_visible(v))
        return 0;
    int lh = line_h(w);
    r->x = 1;
    r->y = 1 + (header_h(v)) + (idx - v->scroll) * lh;
    r->w = w->w - 2;
    r->h = lh;
    return 1;
}

void view_scroll_to(struct widget *w, int row)
{
    struct view *v = (struct view *)w;
    int idx = flat_index_of(v, row);
    if (idx < 0)
        return;
    int rows = rows_visible(v);
    if (idx < v->scroll) v->scroll = idx;
    if (rows > 0 && idx >= v->scroll + rows) v->scroll = idx - rows + 1;
    widget_invalidate(w);
}

int view_scroll_position(const struct widget *w)
{
    return ((const struct view *)w)->scroll;
}

void table_set_column_width(struct widget *w, int col, int width)
{
    struct view *v = (struct view *)w;
    if (col >= 0 && col < v->ncols) {
        v->widths[col] = width;
        widget_invalidate(w);
    }
}

int table_column_width(const struct widget *w, int col)
{
    const struct view *v = (const struct view *)w;
    return col >= 0 && col < v->ncols ? v->widths[col] : 0;
}
