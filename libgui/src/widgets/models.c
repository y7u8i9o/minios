/* Tree view and table over struct model: virtualised rows, expansion,
 * selection, sortable and resizable columns. */
#include <gui/app.h>
#include <gui/model.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

long uptime_ms(void);
void scrollbar_paint_track(struct painter *p, int x, int y, int w, int h, int value, int max, int page, int vertical);

#define INDENT 16
#define HEADER_H 24

struct view {
    struct widget w;
    struct model *m;
    int scroll;                 /* first visible flat row */
    /* tree: expanded row ids, flattened visible rows */
    int *expanded, nexpanded;
    int *flat, *depth, nflat;
    /* table: column widths, sort state, header drag */
    int *widths, ncols;
    int sort_col, sort_desc;
    int drag_col, drag_x0, drag_w0;
    int header;                 /* 1 for tables */
    long click_ms;              /* last left click, for double clicks */
    int click_row;
};

#define ICON_W 20
#define DOUBLE_CLICK_MS 400

static int line_h(const struct widget *w) { return widget_theme(w)->font->height + 6; }

static int is_expanded(struct view *v, int row)
{
    for (int i = 0; i < v->nexpanded; i++)
        if (v->expanded[i] == row)
            return 1;
    return 0;
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
        v->nflat++;
        if (!v->header && is_expanded(v, row))
            flatten(v, row, depth + 1);
    }
}

static void refresh(struct view *v)
{
    v->nflat = 0;
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
    return (v->w.h - 2 - (v->header ? HEADER_H : 0)) / lh;
}

static int clamp_scroll(struct view *v, int s)
{
    int top = v->nflat - rows_visible(v);
    if (top < 0) top = 0;
    return s < 0 ? 0 : s > top ? top : s;
}

static int flat_index_of(struct view *v, int row)
{
    for (int i = 0; i < v->nflat; i++)
        if (v->flat[i] == row)
            return i;
    return -1;
}

static void select_flat(struct view *v, int idx, const char *signal)
{
    if (idx < 0 || idx >= v->nflat)
        return;
    v->w.value = v->flat[idx];
    int rows = rows_visible(v);
    if (idx < v->scroll) v->scroll = idx;
    if (rows > 0 && idx >= v->scroll + rows) v->scroll = idx - rows + 1;
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

static void view_paint(struct widget *w, struct painter *p)
{
    struct view *v = (struct view *)w;
    const struct theme *t = p->theme;
    int lh = line_h(w), rows = rows_visible(v);
    int sbw = v->nflat > rows ? theme_px(t, TM_SCROLLBAR) : 0;
    int top = v->header ? HEADER_H : 0;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    char buf[256];
    if (v->header) {
        painter_push(p, 1, 1, w->w - 2 - sbw, HEADER_H);
        painter_fill(p, 0, 0, w->w, HEADER_H, t->color[TC_WINDOW]);
        int x = 0;
        for (int c = 0; c < v->ncols; c++) {
            const char *hd = v->m && v->m->header ? v->m->header(v->m, c) : "";
            painter_push(p, x, 0, v->widths[c], HEADER_H);
            painter_text(p, 4, (HEADER_H - painter_text_height(p)) / 2, hd ? hd : "", t->color[TC_TEXT]);
            if (c == v->sort_col)
                painter_text(p, v->widths[c] - 14, (HEADER_H - painter_text_height(p)) / 2, v->sort_desc ? "v" : "^", t->color[TC_TEXT]);
            painter_pop(p);
            x += v->widths[c];
            painter_line(p, x - 1, 0, x - 1, HEADER_H - 1, t->color[TC_BORDER]);
        }
        painter_line(p, 0, HEADER_H - 1, w->w, HEADER_H - 1, t->color[TC_BORDER]);
        painter_pop(p);
    }
    painter_push(p, 1, 1 + top, w->w - 2 - sbw, w->h - 2 - top);
    for (int i = 0; i <= rows && v->scroll + i < v->nflat; i++) {
        int idx = v->scroll + i, row = v->flat[idx];
        int y = i * lh;
        int selected = row == w->value;
        if (selected)
            painter_fill(p, 0, y, w->w, lh, t->color[TC_SELECTION]);
        uint32_t fg = selected ? t->color[TC_SELECTION_TEXT] : t->color[TC_TEXT];
        int ty = y + (lh - painter_text_height(p)) / 2;
        const struct image *icon = v->m->icon ? v->m->icon(v->m, row) : NULL;
        if (v->header) {
            int x = 0;
            for (int c = 0; c < v->ncols; c++) {
                const char *s = v->m->cell(v->m, row, c, buf, sizeof buf);
                painter_push(p, x, y, v->widths[c] - 1, lh);
                int tx = 4;
                if (c == 0 && icon) {
                    painter_image(p, 4, (lh - image_lh(icon)) / 2, icon);
                    tx += ICON_W;
                }
                painter_text(p, tx, ty - y, s ? s : "", fg);
                painter_pop(p);
                x += v->widths[c];
            }
        } else {
            int x = 4 + v->depth[idx] * INDENT;
            if (icon) {
                painter_image(p, x + INDENT, y + (lh - image_lh(icon)) / 2, icon);
                x += ICON_W;
            }
            if (v->m->rows(v->m, row) > 0) {
                int ex = is_expanded(v, row);
                int cx = x + 4, cy = y + lh / 2;
                for (int k = 0; k < 5; k++) {
                    if (ex)
                        painter_line(p, cx + k, cy - 2 + k, cx + 8 - k, cy - 2 + k, fg);
                    else
                        painter_line(p, cx + k, cy - 4 + k, cx + k, cy + 4 - k, fg);
                }
            }
            const char *s = v->m->cell(v->m, row, 0, buf, sizeof buf);
            painter_text(p, x + INDENT, ty, s ? s : "", fg);
        }
    }
    painter_pop(p);
    if (sbw)
        scrollbar_paint_track(p, w->w - sbw, top, sbw, w->h - top, v->scroll, v->nflat, rows, 1);
}

static void toggle_expand(struct view *v, int row)
{
    int found = -1;
    for (int i = 0; i < v->nexpanded; i++)
        if (v->expanded[i] == row)
            found = i;
    if (found >= 0) {
        memmove(v->expanded + found, v->expanded + found + 1, (size_t)(v->nexpanded - found - 1) * sizeof *v->expanded);
        v->nexpanded--;
    } else {
        v->expanded = realloc(v->expanded, (size_t)(v->nexpanded + 1) * sizeof *v->expanded);
        v->expanded[v->nexpanded++] = row;
    }
    refresh(v);
}

static int view_event(struct widget *w, struct event *e)
{
    struct view *v = (struct view *)w;
    int lh = line_h(w), rows = rows_visible(v);
    int top = v->header ? HEADER_H : 0;
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
        int sbw = v->nflat > rows ? theme_px(widget_theme(w), TM_SCROLLBAR) : 0;
        if (sbw && e->x >= w->w - sbw) {
            int mid = top + 1 + (w->h - 2 - top) * (v->scroll + rows / 2) / (v->nflat ? v->nflat : 1);
            v->scroll = clamp_scroll(v, v->scroll + (e->y < mid ? -rows : rows));
            widget_invalidate(w);
            return 1;
        }
        if (v->header && e->y < HEADER_H + 1) {
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
            int ex = 4 + v->depth[idx] * INDENT;
            if (e->x - 1 >= ex && e->x - 1 < ex + INDENT && v->m->rows(v->m, v->flat[idx]) > 0) {
                toggle_expand(v, v->flat[idx]);
                return 1;
            }
        }
        long now = uptime_ms();
        int again = idx == v->click_row && now - v->click_ms < DOUBLE_CLICK_MS;
        v->click_row = idx;
        v->click_ms = again ? 0 : now;
        select_flat(v, idx, again ? "activate" : "selected");
        return 1;
    }
    case EV_MOUSE_MOVE:
        if (v->drag_col >= 0 && (e->button & 1)) {
            int nw = v->drag_w0 + e->x - v->drag_x0;
            v->widths[v->drag_col] = nw < 20 ? 20 : nw;
            widget_invalidate(w);
            return 1;
        }
        return 0;
    case EV_MOUSE_UP:
        v->drag_col = -1;
        return 1;
    case EV_MOUSE_WHEEL:
        v->scroll = clamp_scroll(v, v->scroll + 3 * e->button);
        widget_invalidate(w);
        return 1;
    case EV_KEY_DOWN: {
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
    free(v->expanded);
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
    v->nexpanded = 0;
    v->scroll = 0;
    w->value = -1;
    refresh(v);
}

void view_refresh(struct widget *w)
{
    struct view *v = (struct view *)w;
    refresh(v);
    v->scroll = clamp_scroll(v, v->scroll);
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
