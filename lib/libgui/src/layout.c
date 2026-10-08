/* Measurement and layout of boxes and grids. Measurement runs bottom
 * up and fills widget->measured from the class hint and the children;
 * layout runs top down placing children inside their parent. */
#include <gui/app.h>
#include <stdlib.h>
#include <string.h>

/* Padding of a container: explicit, else the theme padding for top
 * level windows and none for nested containers. */
static int padding_of(const struct widget *w)
{
    if (w->padding >= 0)
        return w->padding;
    return w->parent ? 0 : theme_px(widget_theme(w), TM_PADDING);
}

/* Combine the application's hint with the class measurement. A widget
 * without needs_measure retains its last measurement. A changed
 * measurement makes the parent place its children again. */
void widget_measure(struct widget *w);
void widget_measure(struct widget *w)
{
    if (!w->needs_measure)
        return;
    w->needs_measure = 0;
    struct size_hint h = { 0 };
    gui_count(GUI_COUNT_LAYOUTS, 1);
    if (w->cls->measure)
        w->cls->measure(w, &h);
    if (w->hint.pref_w) h.pref_w = w->hint.pref_w;
    if (w->hint.pref_h) h.pref_h = w->hint.pref_h;
    if (w->hint.min_w) h.min_w = w->hint.min_w;
    if (w->hint.min_h) h.min_h = w->hint.min_h;
    if (w->hint.max_w) h.max_w = w->hint.max_w;
    if (w->hint.max_h) h.max_h = w->hint.max_h;
    if (h.pref_w < h.min_w) h.pref_w = h.min_w;
    if (h.pref_h < h.min_h) h.pref_h = h.min_h;
    if (h.max_w && h.pref_w > h.max_w) h.pref_w = h.max_w;
    if (h.max_h && h.pref_h > h.max_h) h.pref_h = h.max_h;
    if (memcmp(&h, &w->measured, sizeof h) == 0)
        return;
    w->measured = h;
    if (w->parent) {
        w->parent->needs_layout = 1;
        for (struct widget *p = w->parent->parent; p; p = p->parent)
            p->child_layout = 1;
    }
}

/* Place a child of the given cell with its alignment and margin. */
static void place(struct widget *c, int cx, int cy, int cw, int ch)
{
    int m = c->margin;
    cx += m;
    cy += m;
    cw -= 2 * m;
    ch -= 2 * m;
    int w = cw, h = ch;
    if (c->align_x != ALIGN_FILL) {
        w = c->measured.pref_w < cw ? c->measured.pref_w : cw;
    } else if (c->measured.max_w && w > c->measured.max_w) {
        w = c->measured.max_w;
    }
    if (c->align_y != ALIGN_FILL) {
        h = c->measured.pref_h < ch ? c->measured.pref_h : ch;
    } else if (c->measured.max_h && h > c->measured.max_h) {
        h = c->measured.max_h;
    }
    int x = cx, y = cy;
    if (c->align_x == ALIGN_CENTER) x = cx + (cw - w) / 2;
    else if (c->align_x == ALIGN_END) x = cx + cw - w;
    if (c->align_y == ALIGN_CENTER) y = cy + (ch - h) / 2;
    else if (c->align_y == ALIGN_END) y = cy + ch - h;
    widget_set_rect(c, x, y, w, h);
}

/* ---- box ---- */

static void box_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    int pad = padding_of(w), sp = theme_px(t, TM_SPACING);
    int vertical = w->value;
    int n = 0, along = 0, across = 0, min_along = 0, min_across = 0;
    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible || c->floating)
            continue;
        widget_measure(c);
        int pw = c->measured.pref_w + 2 * c->margin, ph = c->measured.pref_h + 2 * c->margin;
        int mw = c->measured.min_w + 2 * c->margin, mh = c->measured.min_h + 2 * c->margin;
        if (vertical) {
            along += ph;
            min_along += mh;
            if (pw > across) across = pw;
            if (mw > min_across) min_across = mw;
        } else {
            along += pw;
            min_along += mw;
            if (ph > across) across = ph;
            if (mh > min_across) min_across = mh;
        }
        n++;
    }
    int gaps = n > 1 ? (n - 1) * sp : 0;
    if (vertical) {
        h->pref_w = across + 2 * pad;
        h->pref_h = along + gaps + 2 * pad;
        h->min_w = min_across + 2 * pad;
        h->min_h = min_along + gaps + 2 * pad;
    } else {
        h->pref_w = along + gaps + 2 * pad;
        h->pref_h = across + 2 * pad;
        h->min_w = min_along + gaps + 2 * pad;
        h->min_h = min_across + 2 * pad;
    }
}

static void box_layout(struct widget *w)
{
    const struct theme *t = widget_theme(w);
    int pad = padding_of(w), sp = theme_px(t, TM_SPACING);
    int vertical = w->value;
    int n = 0, total_pref = 0, total_stretch = 0;
    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible || c->floating)
            continue;
        n++;
        total_pref += (vertical ? c->measured.pref_h : c->measured.pref_w) + 2 * c->margin;
        total_stretch += vertical ? c->stretch_y : c->stretch_x;
    }
    int avail = (vertical ? w->h : w->w) - 2 * pad - (n > 1 ? (n - 1) * sp : 0);
    int extra = avail - total_pref;
    /* Shrink proportionally when there is not enough room, never below
     * the minimum. */
    int pos = pad;
    int stretch_left = total_stretch, extra_left = extra > 0 ? extra : 0;
    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible || c->floating)
            continue;
        int pref = (vertical ? c->measured.pref_h : c->measured.pref_w) + 2 * c->margin;
        int min = (vertical ? c->measured.min_h : c->measured.min_w) + 2 * c->margin;
        int size = pref;
        int st = vertical ? c->stretch_y : c->stretch_x;
        if (extra > 0 && st > 0) {
            int share = extra_left * st / stretch_left;
            size += share;
            extra_left -= share;
            stretch_left -= st;
        } else if (extra < 0 && total_pref > 0) {
            size = pref + extra * pref / total_pref;
            if (size < min)
                size = min;
        }
        if (vertical)
            place(c, pad, pos, w->w - 2 * pad, size);
        else
            place(c, pos, pad, size, w->h - 2 * pad);
        pos += size + sp;
    }
}

/* ---- grid ---- */

struct grid {
    struct widget w;
    int *row_stretch, *col_stretch;
    int nrows, ncols;
};

static void grid_extent(struct widget *w, int *rows, int *cols)
{
    int r = 0, c = 0;
    for (struct widget *ch = w->first; ch; ch = ch->next) {
        if (!ch->visible || ch->floating)
            continue;
        if (ch->row + ch->row_span > r) r = ch->row + ch->row_span;
        if (ch->col + ch->col_span > c) c = ch->col + ch->col_span;
    }
    *rows = r;
    *cols = c;
}

/* Preferred sizes of rows and columns: single span children set them,
 * spanning children only enlarge when the span is too small. */
static void grid_sizes(struct widget *w, int *rw, int *rh, int rows, int cols, int min)
{
    memset(rw, 0, (size_t)cols * sizeof *rw);
    memset(rh, 0, (size_t)rows * sizeof *rh);
    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible || c->floating)
            continue;
        int cw = (min ? c->measured.min_w : c->measured.pref_w) + 2 * c->margin;
        int ch = (min ? c->measured.min_h : c->measured.pref_h) + 2 * c->margin;
        if (c->col_span == 1 && cw > rw[c->col]) rw[c->col] = cw;
        if (c->row_span == 1 && ch > rh[c->row]) rh[c->row] = ch;
    }
    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible || c->floating)
            continue;
        int cw = (min ? c->measured.min_w : c->measured.pref_w) + 2 * c->margin;
        int ch = (min ? c->measured.min_h : c->measured.pref_h) + 2 * c->margin;
        if (c->col_span > 1) {
            int sum = 0;
            for (int i = 0; i < c->col_span; i++) sum += rw[c->col + i];
            if (sum < cw) rw[c->col + c->col_span - 1] += cw - sum;
        }
        if (c->row_span > 1) {
            int sum = 0;
            for (int i = 0; i < c->row_span; i++) sum += rh[c->row + i];
            if (sum < ch) rh[c->row + c->row_span - 1] += ch - sum;
        }
    }
}

static void grid_measure(struct widget *w, struct size_hint *h)
{
    const struct theme *t = widget_theme(w);
    int pad = padding_of(w), sp = theme_px(t, TM_SPACING);
    for (struct widget *c = w->first; c; c = c->next)
        if (c->visible)
            widget_measure(c);
    int rows, cols;
    grid_extent(w, &rows, &cols);
    if (!rows || !cols) {
        h->pref_w = h->pref_h = 2 * pad;
        return;
    }
    int *rw = calloc((size_t)cols, sizeof *rw), *rh = calloc((size_t)rows, sizeof *rh);
    for (int pass = 0; pass < 2; pass++) {
        grid_sizes(w, rw, rh, rows, cols, pass);
        int tw = 2 * pad + (cols - 1) * sp, th = 2 * pad + (rows - 1) * sp;
        for (int i = 0; i < cols; i++) tw += rw[i];
        for (int i = 0; i < rows; i++) th += rh[i];
        if (pass == 0) { h->pref_w = tw; h->pref_h = th; }
        else { h->min_w = tw; h->min_h = th; }
    }
    free(rw);
    free(rh);
}

static void distribute(int *sizes, const int *stretch, int n, int extra)
{
    int total = 0;
    for (int i = 0; i < n; i++)
        total += stretch ? stretch[i] : 0;
    if (extra > 0 && total > 0) {
        int left = extra, sleft = total;
        for (int i = 0; i < n; i++) {
            if (!stretch[i])
                continue;
            int share = left * stretch[i] / sleft;
            sizes[i] += share;
            left -= share;
            sleft -= stretch[i];
        }
    } else if (extra < 0) {
        int sum = 0;
        for (int i = 0; i < n; i++) sum += sizes[i];
        if (sum > 0)
            for (int i = 0; i < n; i++)
                sizes[i] += extra * sizes[i] / sum;
    }
}

static void grid_layout(struct widget *w)
{
    struct grid *g = (struct grid *)w;
    const struct theme *t = widget_theme(w);
    int pad = padding_of(w), sp = theme_px(t, TM_SPACING);
    int rows, cols;
    grid_extent(w, &rows, &cols);
    if (!rows || !cols)
        return;
    int *rw = calloc((size_t)cols, sizeof *rw), *rh = calloc((size_t)rows, sizeof *rh);
    int *cs = calloc((size_t)cols, sizeof *cs), *rs = calloc((size_t)rows, sizeof *rs);
    grid_sizes(w, rw, rh, rows, cols, 0);
    for (int i = 0; i < cols; i++) cs[i] = i < g->ncols ? g->col_stretch[i] : 0;
    for (int i = 0; i < rows; i++) rs[i] = i < g->nrows ? g->row_stretch[i] : 0;
    /* Children with stretch factors stretch their own column or row. */
    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible || c->floating)
            continue;
        if (c->stretch_x && c->col_span == 1 && !cs[c->col]) cs[c->col] = c->stretch_x;
        if (c->stretch_y && c->row_span == 1 && !rs[c->row]) rs[c->row] = c->stretch_y;
    }
    int tw = (cols - 1) * sp, th = (rows - 1) * sp;
    for (int i = 0; i < cols; i++) tw += rw[i];
    for (int i = 0; i < rows; i++) th += rh[i];
    distribute(rw, cs, cols, w->w - 2 * pad - tw);
    distribute(rh, rs, rows, w->h - 2 * pad - th);
    int *cx = calloc((size_t)cols + 1, sizeof *cx), *cy = calloc((size_t)rows + 1, sizeof *cy);
    cx[0] = pad;
    for (int i = 0; i < cols; i++) cx[i + 1] = cx[i] + rw[i] + sp;
    cy[0] = pad;
    for (int i = 0; i < rows; i++) cy[i + 1] = cy[i] + rh[i] + sp;
    for (struct widget *c = w->first; c; c = c->next) {
        if (!c->visible || c->floating)
            continue;
        int x0 = cx[c->col], x1 = cx[c->col + c->col_span] - sp;
        int y0 = cy[c->row], y1 = cy[c->row + c->row_span] - sp;
        place(c, x0, y0, x1 - x0, y1 - y0);
    }
    free(rw); free(rh); free(cs); free(rs); free(cx); free(cy);
}

static void grid_destroy(struct widget *w)
{
    struct grid *g = (struct grid *)w;
    free(g->row_stretch);
    free(g->col_stretch);
}

void grid_set_stretch(struct widget *w, int row, int col, int stretch)
{
    struct grid *g = (struct grid *)w;
    if (row >= 0) {
        if (row >= g->nrows) {
            g->row_stretch = realloc(g->row_stretch, (size_t)(row + 1) * sizeof *g->row_stretch);
            for (int i = g->nrows; i <= row; i++) g->row_stretch[i] = 0;
            g->nrows = row + 1;
        }
        g->row_stretch[row] = stretch;
    }
    if (col >= 0) {
        if (col >= g->ncols) {
            g->col_stretch = realloc(g->col_stretch, (size_t)(col + 1) * sizeof *g->col_stretch);
            for (int i = g->ncols; i <= col; i++) g->col_stretch[i] = 0;
            g->ncols = col + 1;
        }
        g->col_stretch[col] = stretch;
    }
    widget_relayout(w);
}

/* A transparent box paints no background. */
static void container_paint(struct widget *w, struct painter *p)
{
    if (!w->transparent)
        painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_WINDOW]);
}

const struct widget_class box_class = { "box", sizeof(struct widget), box_measure, box_layout, container_paint, NULL, NULL };
const struct widget_class grid_class = { "grid", sizeof(struct grid), grid_measure, grid_layout, container_paint, NULL, grid_destroy };

struct widget *box_new(struct widget *parent, int vertical)
{
    struct widget *w = widget_new(&box_class, parent);
    if (w)
        w->value = vertical;
    return w;
}

struct widget *grid_new(struct widget *parent)
{
    return widget_new(&grid_class, parent);
}
