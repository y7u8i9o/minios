/* Text editor widget: an array of lines, a cursor with a selection
 * anchor, an undo and redo stack of insert and delete operations,
 * optional word wrap and line numbers, search, a highlighter and the
 * clipboard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gui/utf8.h>
#include "editor_internal.h"
#include <gui/pixel.h>
#include "editmenu.h"

/* ---- lines ---- */

void lines_insert(struct editor *ed, int at, char *line)
{
    ed->lines = realloc(ed->lines, (size_t)(ed->nlines + 1) * sizeof *ed->lines);
    memmove(ed->lines + at + 1, ed->lines + at, (size_t)(ed->nlines - at) * sizeof *ed->lines);
    ed->lines[at] = line;
    ed->nlines++;
}

static void lines_delete(struct editor *ed, int at)
{
    free(ed->lines[at]);
    memmove(ed->lines + at, ed->lines + at + 1, (size_t)(ed->nlines - at - 1) * sizeof *ed->lines);
    ed->nlines--;
}

int llen(const struct editor *ed, int l) { return (int)strlen(ed->lines[l]); }

static void invalidate_rows(struct editor *ed, int r0, int r1);

/* An edit inside one line repaints the rows of that line, and with
 * wrapping the rows below it. Other edits repaint the editor. */
void editor_changed(struct editor *ed)
{
    ed->modified = 1;
    if (!ed->rows_dirty && ed->edit_old == 1 && ed->edit_new == 1 && ed->first_row) {
        int r = ed->first_row[ed->edit_line];
        invalidate_rows(ed, r, ed->wrap ? ed->scroll + ed->w.h : r + 1);
    } else {
        widget_invalidate(&ed->w);
    }
    struct sig_change c = { ed->cl, NULL };
    widget_emit(&ed->w, "changed", &c);
}

/* Raw insert of text (may contain newlines) at (l, c); *el, *ec receive
 * the end position. */
static void raw_insert(struct editor *ed, int l, int c, const char *text, int n, int *el, int *ec)
{
    int breaks = 0;
    for (int i = 0; i < n; i++)
        breaks += text[i] == '\n';
    lines_changed(ed, l, 1, 1 + breaks);
    char *line = ed->lines[l];
    int len = llen(ed, l);
    char *tail = strdup(line + c);
    line[c] = '\0';
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && text[j] != '\n')
            j++;
        int seg = j - i;
        int cur = (int)strlen(ed->lines[l]);
        ed->lines[l] = realloc(ed->lines[l], (size_t)(cur + seg + 1));
        memcpy(ed->lines[l] + cur, text + i, (size_t)seg);
        ed->lines[l][cur + seg] = '\0';
        if (j < n) {
            lines_insert(ed, l + 1, strdup(""));
            l++;
        }
        i = j + 1;
    }
    *el = l;
    *ec = (int)strlen(ed->lines[l]);
    int cur = *ec;
    ed->lines[l] = realloc(ed->lines[l], (size_t)(cur + strlen(tail) + 1));
    strcpy(ed->lines[l] + cur, tail);
    free(tail);
    (void)len;
}

/* copy_range returns the text of the range as a malloc'ed string. */
static char *copy_range(struct editor *ed, int l0, int c0, int l1, int c1)
{
    size_t cap = 64, n = 0;
    char *out = malloc(cap);
    for (int l = l0; l <= l1; l++) {
        int s = l == l0 ? c0 : 0, e = l == l1 ? c1 : llen(ed, l);
        int seg = e - s;
        if (n + (size_t)seg + 2 > cap) {
            cap = (n + (size_t)seg + 2) * 2;
            out = realloc(out, cap);
        }
        memcpy(out + n, ed->lines[l] + s, (size_t)seg);
        n += (size_t)seg;
        if (l < l1)
            out[n++] = '\n';
    }
    out[n] = '\0';
    return out;
}

/* raw_delete removes the range and returns the removed text. */
static char *raw_delete(struct editor *ed, int l0, int c0, int l1, int c1)
{
    lines_changed(ed, l0, l1 - l0 + 1, 1);
    char *out = copy_range(ed, l0, c0, l1, c1);
    char *tail = strdup(ed->lines[l1] + c1);
    ed->lines[l0][c0] = '\0';
    ed->lines[l0] = realloc(ed->lines[l0], (size_t)c0 + strlen(tail) + 1);
    strcpy(ed->lines[l0] + c0, tail);
    free(tail);
    for (int l = l1; l > l0; l--)
        lines_delete(ed, l);
    return out;
}

/* ---- undo ---- */

static void push(struct op **stack, int *n, struct op op)
{
    *stack = realloc(*stack, (size_t)(*n + 1) * sizeof **stack);
    (*stack)[(*n)++] = op;
}

void clear_ops(struct op **stack, int *n)
{
    for (int i = 0; i < *n; i++)
        free((*stack)[i].text);
    free(*stack);
    *stack = NULL;
    *n = 0;
}

static void insert_text(struct editor *ed, const char *text, int n, int mergeable)
{
    int el, ec;
    raw_insert(ed, ed->cl, ed->cc, text, n, &el, &ec);
    if (mergeable && ed->merge && ed->nundo && !ed->group) {
        struct op *last = &ed->undo[ed->nundo - 1];
        if (last->kind == 0) {
            /* End position of the last insert must equal the cursor. */
            int l = last->l, c = last->c;
            for (int i = 0; i < last->len; i++)
                if (last->text[i] == '\n') { l++; c = 0; } else c++;
            if (l == ed->cl && c == ed->cc) {
                last->text = realloc(last->text, (size_t)(last->len + n + 1));
                memcpy(last->text + last->len, text, (size_t)n);
                last->len += n;
                last->text[last->len] = '\0';
                ed->cl = el;
                ed->cc = ec;
                clear_ops(&ed->redo, &ed->nredo);
                editor_changed(ed);
                return;
            }
        }
    }
    struct op op = { 0, ed->cl, ed->cc, malloc((size_t)n + 1), n, ed->group };
    memcpy(op.text, text, (size_t)n);
    op.text[n] = '\0';
    push(&ed->undo, &ed->nundo, op);
    clear_ops(&ed->redo, &ed->nredo);
    ed->merge = mergeable;
    ed->cl = el;
    ed->cc = ec;
    editor_changed(ed);
}

static void delete_range(struct editor *ed, int l0, int c0, int l1, int c1)
{
    if (l0 > l1 || (l0 == l1 && c0 > c1)) {
        int t = l0; l0 = l1; l1 = t;
        t = c0; c0 = c1; c1 = t;
    }
    if (l0 == l1 && c0 == c1)
        return;
    char *text = raw_delete(ed, l0, c0, l1, c1);
    struct op op = { 1, l0, c0, text, (int)strlen(text), ed->group };
    push(&ed->undo, &ed->nundo, op);
    clear_ops(&ed->redo, &ed->nredo);
    ed->merge = 0;
    ed->cl = l0;
    ed->cc = c0;
    ed->has_sel = 0;
    editor_changed(ed);
}

static void apply(struct editor *ed, struct op *op, struct op **to, int *nto)
{
    if (op->kind == 0) {
        int l = op->l, c = op->c;
        for (int i = 0; i < op->len; i++)
            if (op->text[i] == '\n') { l++; c = 0; } else c++;
        free(raw_delete(ed, op->l, op->c, l, c));
        struct op inv = { 1, op->l, op->c, op->text, op->len, op->group };
        push(to, nto, inv);
        ed->cl = op->l;
        ed->cc = op->c;
    } else {
        int el, ec;
        raw_insert(ed, op->l, op->c, op->text, op->len, &el, &ec);
        struct op inv = { 0, op->l, op->c, op->text, op->len, op->group };
        push(to, nto, inv);
        ed->cl = el;
        ed->cc = ec;
    }
    ed->has_sel = 0;
    ed->merge = 0;
    editor_changed(ed);
}

/* replay moves the operation on top of one stack to the other, together
 * with the operations below it that belong to the same group. */
static int replay(struct editor *ed, struct op **from, int *nfrom, struct op **to, int *nto)
{
    if (!*nfrom)
        return 0;
    int group;
    do {
        struct op op = (*from)[--*nfrom];
        group = op.group;
        apply(ed, &op, to, nto);
    } while (group && *nfrom && (*from)[*nfrom - 1].group == group);
    return 1;
}

int editor_undo(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    return replay(ed, &ed->undo, &ed->nundo, &ed->redo, &ed->nredo);
}

int editor_redo(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    return replay(ed, &ed->redo, &ed->nredo, &ed->undo, &ed->nundo);
}

int editor_can_undo(const struct widget *w) { return ((const struct editor *)w)->nundo > 0; }
int editor_can_redo(const struct widget *w) { return ((const struct editor *)w)->nredo > 0; }

/* group_begin starts an undo group for an operation that consists of
 * several inserts and deletes, and group_end closes it. */
static void group_begin(struct editor *ed)
{
    ed->group = ++ed->next_group;
    ed->merge = 0;
}

static void group_end(struct editor *ed)
{
    ed->group = 0;
}

/* ---- selection ---- */

static void sel_range(struct editor *ed, int *l0, int *c0, int *l1, int *c1)
{
    if (!ed->has_sel || (ed->al == ed->cl && ed->ac == ed->cc)) {
        *l0 = *l1 = ed->cl;
        *c0 = *c1 = ed->cc;
        return;
    }
    if (ed->al < ed->cl || (ed->al == ed->cl && ed->ac < ed->cc)) {
        *l0 = ed->al; *c0 = ed->ac; *l1 = ed->cl; *c1 = ed->cc;
    } else {
        *l0 = ed->cl; *c0 = ed->cc; *l1 = ed->al; *c1 = ed->ac;
    }
}

static int delete_selection(struct editor *ed)
{
    int l0, c0, l1, c1;
    sel_range(ed, &l0, &c0, &l1, &c1);
    ed->has_sel = 0;
    if (l0 == l1 && c0 == c1)
        return 0;
    delete_range(ed, l0, c0, l1, c1);
    return 1;
}

int editor_has_selection(const struct widget *w)
{
    const struct editor *ed = (const struct editor *)w;
    return ed->has_sel && (ed->al != ed->cl || ed->ac != ed->cc);
}

char *editor_selection(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    if (!editor_has_selection(w))
        return NULL;
    int l0, c0, l1, c1;
    sel_range(ed, &l0, &c0, &l1, &c1);
    return copy_range(ed, l0, c0, l1, c1);
}

void editor_copy(struct widget *w)
{
    char *text = editor_selection(w);
    if (text) {
        gui_clipboard_set(text, (int)strlen(text));
        free(text);
    }
}

void editor_cut(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    if (ed->readonly || !editor_has_selection(w))
        return;
    editor_copy(w);
    delete_selection(ed);
    cursor_moved(ed);
}

void editor_paste(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    if (ed->readonly)
        return;
    char *tmp = malloc(WSRV_CLIP_MAX);
    int n = tmp ? gui_clipboard_get(tmp, WSRV_CLIP_MAX) : -1;
    if (n > 0) {
        group_begin(ed);
        delete_selection(ed);
        insert_text(ed, tmp, n, 0);
        group_end(ed);
        cursor_moved(ed);
    }
    free(tmp);
}

void editor_delete_selection(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    if (!ed->readonly && delete_selection(ed))
        cursor_moved(ed);
}

void editor_select_all(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    ed->al = 0;
    ed->ac = 0;
    ed->has_sel = 1;
    ed->cl = ed->nlines - 1;
    ed->cc = llen(ed, ed->cl);
    cursor_moved(ed);
}

int editor_replace_all(struct widget *w, const char *needle, const char *replacement)
{
    struct editor *ed = (struct editor *)w;
    int n = (int)strlen(needle), rn = (int)strlen(replacement), count = 0;
    if (ed->readonly || n == 0 || strchr(needle, '\n'))
        return 0;
    group_begin(ed);
    for (int l = 0; l < ed->nlines; l++) {
        int c = 0;
        char *hit;
        while ((hit = strstr(ed->lines[l] + c, needle)) != NULL) {
            int at = (int)(hit - ed->lines[l]);
            delete_range(ed, l, at, l, at + n);
            ed->cl = l;
            ed->cc = at;
            if (rn)
                insert_text(ed, replacement, rn, 0);
            c = at + rn;
            count++;
        }
    }
    group_end(ed);
    if (count)
        cursor_moved(ed);
    return count;
}

/* ---- words ---- */

/* select_word selects the word at the cursor, and select_line selects the
 * line of the cursor including its newline. */
static void select_word(struct editor *ed)
{
    int a, b;
    gui_word_at(ed->lines[ed->cl], llen(ed, ed->cl), ed->cc, &a, &b);
    ed->al = ed->cl;
    ed->ac = a;
    ed->cc = b;
    ed->has_sel = 1;
}

static void select_line(struct editor *ed)
{
    ed->al = ed->cl;
    ed->ac = 0;
    ed->has_sel = 1;
    if (ed->cl + 1 < ed->nlines) {
        ed->cl++;
        ed->cc = 0;
    } else {
        ed->cc = llen(ed, ed->cl);
    }
}

/* indent_lines inserts four spaces at the start of the lines l0 to l1, or
 * removes up to four leading spaces from them when out is set.  The lines
 * remain selected. */
static void indent_lines(struct editor *ed, int l0, int l1, int out)
{
    group_begin(ed);
    for (int l = l0; l <= l1; l++) {
        if (out) {
            int k = 0;
            while (k < 4 && ed->lines[l][k] == ' ')
                k++;
            if (k)
                delete_range(ed, l, 0, l, k);
        } else if (ed->lines[l][0]) {
            ed->cl = l;
            ed->cc = 0;
            insert_text(ed, "    ", 4, 0);
        }
    }
    group_end(ed);
    ed->al = l0;
    ed->ac = 0;
    ed->cl = l1;
    ed->cc = llen(ed, l1);
    ed->has_sel = 1;
}

/* ---- visual rows ---- */

/* Makes room for n more rows. */
static void reserve_rows(struct editor *ed, int n)
{
    if (ed->nrows + n <= ed->rows_cap)
        return;
    int cap = ed->rows_cap ? ed->rows_cap : 128;
    while (cap < ed->nrows + n)
        cap *= 2;
    ed->row_line = realloc(ed->row_line, (size_t)cap * sizeof *ed->row_line);
    ed->row_start = realloc(ed->row_start, (size_t)cap * sizeof *ed->row_start);
    ed->row_len = realloc(ed->row_len, (size_t)cap * sizeof *ed->row_len);
    ed->rows_cap = cap;
}

static void add_row(struct editor *ed, int line, int start, int len)
{
    reserve_rows(ed, 1);
    ed->row_line[ed->nrows] = line;
    ed->row_start[ed->nrows] = start;
    ed->row_len[ed->nrows] = len;
    ed->nrows++;
}

void lines_changed(struct editor *ed, int l, int old_n, int new_n)
{
    if (ed->hl_valid > l)
        ed->hl_valid = l;
    if (ed->rows_dirty)
        return;
    if (ed->edit_old >= 0) {
        ed->rows_dirty = 1;
        return;
    }
    ed->edit_line = l;
    ed->edit_old = old_n;
    ed->edit_new = new_n;
}

static int gutter_w(struct editor *ed)
{
    if (!ed->numbers)
        return 0;
    char digits[16];
    snprintf(digits, sizeof digits, "%d", ed->nlines > 0 ? ed->nlines : 1);
    memset(digits, '0', strlen(digits));
    return widget_text_width(&ed->w, ed->font, digits, -1) + 10;
}

static int text_w(struct editor *ed)
{
    int sb = theme_px(widget_theme(&ed->w), TM_SCROLLBAR);
    return ed->w.w - 2 - gutter_w(ed) - 2 * PAD - sb;
}

/* Appends the rows of line l for a text width of avail. */
static void wrap_line(struct editor *ed, int l, int avail)
{
    const char *s = ed->lines[l];
    int len = (int)strlen(s);
    if (!ed->wrap || avail < 40 || len == 0) {
        add_row(ed, l, 0, len);
        return;
    }
    int start = 0;
    while (start < len) {
        int fit = widget_text_index(&ed->w, ed->font, s + start, len - start, avail);
        if (fit <= 0)
            fit = 1;
        if (start + fit < len) {
            /* Break after the last space that fits. */
            int k = fit;
            while (k > 1 && s[start + k - 1] != ' ')
                k--;
            if (k > 1)
                fit = k;
        }
        add_row(ed, l, start, fit);
        start += fit;
    }
}

static void build_rows(struct editor *ed)
{
    int avail = text_w(ed);
    ed->nrows = 0;
    ed->first_row = realloc(ed->first_row, (size_t)(ed->nlines + 1) * sizeof *ed->first_row);
    for (int l = 0; l < ed->nlines; l++) {
        ed->first_row[l] = ed->nrows;
        wrap_line(ed, l, avail);
    }
    ed->first_row[ed->nlines] = ed->nrows;
    ed->rows_dirty = 0;
    ed->edit_old = -1;
    ed->rows_width = avail;
}

/* Replaces the rows of the edited lines and moves the rows after them.
 * Only the edited lines are wrapped again. */
static void update_rows(struct editor *ed)
{
    int l = ed->edit_line, old_n = ed->edit_old, new_n = ed->edit_new, delta = new_n - old_n;
    ed->edit_old = -1;
    int r0 = ed->first_row[l], r1 = ed->first_row[l + old_n], total = ed->nrows;
    /* Wrap the new lines after the existing rows, then move them into
     * place. */
    ed->nrows = total;
    for (int k = 0; k < new_n; k++)
        wrap_line(ed, l + k, ed->rows_width);
    int added = ed->nrows - total, tail = total - r1;
    reserve_rows(ed, added);
    int *arrays[3] = { ed->row_line, ed->row_start, ed->row_len };
    for (int a = 0; a < 3; a++) {
        int *v = arrays[a];
        int *fresh = malloc((size_t)(added ? added : 1) * sizeof *fresh);
        memcpy(fresh, v + total, (size_t)added * sizeof *fresh);
        memmove(v + r0 + added, v + r1, (size_t)tail * sizeof *v);
        memcpy(v + r0, fresh, (size_t)added * sizeof *fresh);
        free(fresh);
    }
    ed->nrows = r0 + added + tail;
    for (int r = r0 + added; r < ed->nrows; r++)
        ed->row_line[r] += delta;
    /* The row index of the lines. */
    size_t after = (size_t)(ed->nlines - (l + new_n) + 1) * sizeof *ed->first_row;
    int *fr = ed->first_row;
    if (delta < 0)
        memmove(fr + l + new_n, fr + l + old_n, after);
    fr = realloc(fr, (size_t)(ed->nlines + 1) * sizeof *fr);
    if (delta > 0)
        memmove(fr + l + new_n, fr + l + old_n, after);
    ed->first_row = fr;
    int r = r0;
    for (int k = 0; k < new_n; k++) {
        fr[l + k] = r;
        while (r < r0 + added && ed->row_line[r] == l + k)
            r++;
    }
    for (int k = l + new_n; k <= ed->nlines; k++)
        fr[k] += added - (r1 - r0);
}

static int row_of(struct editor *ed, int l, int c)
{
    int r = ed->first_row[l], end = ed->first_row[l + 1];
    while (r + 1 < end && c >= ed->row_start[r] + ed->row_len[r])
        r++;
    return r;
}

static int rows_visible(struct editor *ed)
{
    int lh = LH(ed);
    return lh ? (ed->w.h - 2) / lh : 1;
}

static void ensure_rows(struct editor *ed)
{
    if (ed->rows_dirty || ed->rows_width != text_w(ed))
        build_rows(ed);
    else if (ed->edit_old >= 0)
        update_rows(ed);
}

static void scroll_to_cursor(struct editor *ed)
{
    ensure_rows(ed);
    int r = row_of(ed, ed->cl, ed->cc), vis = rows_visible(ed);
    if (r < ed->scroll)
        ed->scroll = r;
    if (r >= ed->scroll + vis)
        ed->scroll = r - vis + 1;
    if (!ed->wrap) {
        int cx = widget_text_width(&ed->w, ed->font, ed->lines[ed->cl], ed->cc);
        int avail = text_w(ed);
        if (cx < ed->scroll_x) ed->scroll_x = cx;
        if (cx > ed->scroll_x + avail - 1) ed->scroll_x = cx - avail + 1;
    } else {
        ed->scroll_x = 0;
    }
}

/* Repaints the rows r0 to r1 - 1 where they are visible. */
static void invalidate_rows(struct editor *ed, int r0, int r1)
{
    int lh = LH(ed), sb = theme_px(widget_theme(&ed->w), TM_SCROLLBAR);
    int y0 = 1 + (r0 - ed->scroll) * lh, y1 = 1 + (r1 - ed->scroll) * lh;
    if (y0 < 1)
        y0 = 1;
    if (y1 > y0)
        widget_invalidate_rect(&ed->w, (struct rect){ 1, y0, ed->w.w - 2 - sb, y1 - y0 });
}

/* A move of the cursor repaints the old and the new row of the cursor,
 * which show the caret and the current line. A scroll or a selection
 * repaints the editor. */
void cursor_moved(struct editor *ed)
{
    int scroll = ed->scroll, scroll_x = ed->scroll_x;
    scroll_to_cursor(ed);
    int row = row_of(ed, ed->cl, ed->cc);
    if (ed->scroll != scroll || ed->scroll_x != scroll_x || ed->has_sel || ed->sel_shown || ed->caret_row < 0) {
        widget_invalidate(&ed->w);
    } else {
        invalidate_rows(ed, ed->caret_row, ed->caret_row + 1);
        invalidate_rows(ed, row, row + 1);
    }
    ed->caret_row = row;
    ed->sel_shown = ed->has_sel;
    struct sig_change c = { ed->cl, NULL };
    widget_emit(&ed->w, "cursor", &c);
}

/* ---- painting ---- */

static uint32_t class_color(const struct theme *t, int cls)
{
    return cls > HL_NORMAL && cls < HL_COUNT ? highlight_colors[cls] : t->color[TC_TEXT];
}

/* The current line: 30 percent of the highlight colour over the field. */
static uint32_t current_line_color(const struct theme *t)
{
    return pixel_blend(t->color[TC_FIELD], t->color[TC_HIGHLIGHT], 77) & 0xffffff;
}

/* The state of the highlighter at the start of line l. The cache of the
 * states is extended from its last valid entry. An edit at line N
 * invalidates the entries after N, so a paint near the edit runs the
 * highlighter for the visible lines only. */
static int hl_state_at(struct editor *ed, int l, unsigned char **classes)
{
    ed->hl_state = realloc(ed->hl_state, (size_t)(ed->nlines + 1) * sizeof *ed->hl_state);
    ed->hl_state[0] = 0;
    for (int k = ed->hl_valid; k < l; k++) {
        int len = llen(ed, k), state = ed->hl_state[k];
        *classes = realloc(*classes, (size_t)len + 1);
        ed->hl(ed->lines[k], len, *classes, &state, ed->hl_arg);
        ed->hl_state[k + 1] = state;
    }
    if (ed->hl_valid < l)
        ed->hl_valid = l;
    return ed->hl_state[l];
}

static void editor_paint(struct widget *w, struct painter *p)
{
    struct editor *ed = (struct editor *)w;
    const struct theme *t = p->theme;
    ensure_rows(ed);
    int lh = LH(ed), vis = rows_visible(ed), gw = gutter_w(ed);
    int sb = theme_px(t, TM_SCROLLBAR);
    painter_fill(p, 0, 0, w->w, w->h, t->color[w->enabled ? TC_FIELD : TC_TRACK]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    if (gw) {
        painter_fill(p, 1, 1, gw, w->h - 2, t->color[TC_WINDOW]);
        painter_line(p, gw, 1, gw, w->h - 2, t->color[TC_BORDER]);
    }
    int l0, c0, l1, c1;
    sel_range(ed, &l0, &c0, &l1, &c1);
    unsigned char *classes = NULL;
    /* Only the rows inside the clip, for a partial paint. The highlighter
     * starts at the line of the first painted row. */
    struct rect clip = painter_clip_local(p);
    int i0 = clip.y > 1 ? (clip.y - 1) / lh : 0, i1 = (clip.y + clip.h + lh - 1) / lh;
    /* The text area and the gutter lie left of the track. A clip inside
     * the track paints no row. */
    if (clip.x >= w->w - sb)
        i1 = 0;
    int first_line = ed->scroll + i0 < ed->nrows ? ed->row_line[ed->scroll + i0] : 0;
    int state = ed->hl ? hl_state_at(ed, first_line, &classes) : 0;
    int last_hl_line = -1;
    painter_push(p, 1 + gw, 1, w->w - 2 - gw - sb, w->h - 2);
    for (int i = i0; i <= vis && i < i1 && ed->scroll + i < ed->nrows; i++) {
        int r = ed->scroll + i, l = ed->row_line[r], start = ed->row_start[r], len = ed->row_len[r];
        const char *s = ed->lines[l];
        int y = i * lh, ty = y + 1;
        int x = PAD - ed->scroll_x;
        if (ed->hl && l != last_hl_line) {
            int ll = llen(ed, l);
            classes = realloc(classes, (size_t)ll + 1);
            ed->hl(s, ll, classes, &state, ed->hl_arg);
            last_hl_line = l;
            if (l == ed->hl_valid) {
                ed->hl_state[l + 1] = state;
                ed->hl_valid = l + 1;
            }
        }
        /* The row of the cursor has a faint background while the editor
         * has the focus and no selection. */
        if (w->focused && l == ed->cl && !editor_has_selection(w))
            painter_fill(p, -PAD, y, w->w, lh, current_line_color(t));
        /* Selection background on this row. The selected bytes sa to sb2
         * are drawn in the selection text colour. */
        int sa = -1, sb2 = -1;
        if ((l > l0 || (l == l0 && start + len >= c0)) && (l < l1 || (l == l1 && start <= c1)) && !(l0 == l1 && c0 == c1)) {
            sa = l == l0 && c0 > start ? c0 : start;
            sb2 = l == l1 && c1 < start + len ? c1 : start + len;
            int x0 = x + widget_text_width(w, ed->font, s + start, sa - start);
            int x1 = x + widget_text_width(w, ed->font, s + start, sb2 - start);
            if (l < l1 && sb2 == start + len)
                x1 += widget_text_width(w, ed->font, " ", 1);
            painter_fill(p, x0, y, x1 - x0, lh, t->color[TC_SELECTION]);
        }
        /* Text in runs of one class and one selection state. */
        int j = start;
        char buf[512];
        while (j < start + len) {
            int cls = ed->hl ? classes[j] : 0, sel = j >= sa && j < sb2;
            int k = j;
            while (k < start + len && (ed->hl ? classes[k] : 0) == cls && (k >= sa && k < sb2) == sel &&
                   k - j < (int)sizeof buf - 1)
                k++;
            memcpy(buf, s + j, (size_t)(k - j));
            buf[k - j] = '\0';
            /* Each run starts at the width of the row up to it. The sum
             * of the rounded widths of single runs would drift. */
            painter_text_font(p, ed_font(ed), x + widget_text_width(w, ed->font, s + start, j - start), ty, buf,
                              !w->enabled ? t->color[TC_TEXT_DISABLED] : sel ? t->color[TC_SELECTION_TEXT]
                                                                             : class_color(t, cls),
                              0xffffffffu);
            j = k;
        }
        if (w->focused && !ed->readonly && l == ed->cl && ed->cc >= start && (ed->cc < start + len || (ed->cc == start + len && (r + 1 >= ed->nrows || ed->row_line[r + 1] != l)))) {
            int cx = PAD - ed->scroll_x + widget_text_width(w, ed->font, s + start, ed->cc - start);
            widget_text_cursor(w, 1 + gw + cx, 1 + y, 1, lh);
            if (ed->preedit[0]) {
                painter_text_font(p, ed_font(ed), cx, ty, ed->preedit, t->color[TC_TEXT], 0xffffffffu);
                int pw = widget_text_width(w, ed->font, ed->preedit, -1);
                painter_line(p, cx, y + lh - 1, cx + pw, y + lh - 1, t->color[TC_ACCENT]);
                cx += pw;
            }
            if (widget_caret_visible(w))
                painter_line(p, cx, y, cx, y + lh - 1, t->color[TC_TEXT]);
        }
        if (ed->dropping && l == ed->drop_l && ed->drop_c >= start &&
            (ed->drop_c < start + len || (ed->drop_c == start + len && (r + 1 >= ed->nrows || ed->row_line[r + 1] != l)))) {
            int cx = PAD - ed->scroll_x + widget_text_width(w, ed->font, s + start, ed->drop_c - start);
            painter_fill(p, cx - 1, y, 2, lh, t->color[TC_ACCENT]);
        }
    }
    painter_pop(p);
    free(classes);
    if (gw) {
        painter_push(p, 1, 1, gw - 1, w->h - 2);
        for (int i = i0; i <= vis && i < i1 && ed->scroll + i < ed->nrows; i++) {
            int r = ed->scroll + i;
            if (ed->row_start[r] != 0)
                continue;
            char num[16];
            snprintf(num, sizeof num, "%d", ed->row_line[r] + 1);
            int tw = widget_text_width(w, ed->font, num, -1);
            painter_text_font(p, ed_font(ed), gw - 6 - tw, i * lh + 1, num, t->color[TC_TEXT_DISABLED], 0xffffffffu);
        }
        painter_pop(p);
    }
    scrollbar_paint_track(p, w->w - sb, 0, sb, w->h, ed->scroll, ed->nrows, vis, 1);
}

/* ---- editing keys ---- */

static void move_vertical(struct editor *ed, int rows)
{
    ensure_rows(ed);
    int r = row_of(ed, ed->cl, ed->cc);
    if (ed->wanted_x < 0)
        ed->wanted_x = widget_text_width(&ed->w, ed->font, ed->lines[ed->cl] + ed->row_start[r], ed->cc - ed->row_start[r]);
    int nr = r + rows;
    if (nr < 0) nr = 0;
    if (nr >= ed->nrows) nr = ed->nrows - 1;
    if (nr == r) {
        if (rows < 0) ed->cc = 0;
        else ed->cc = llen(ed, ed->cl);
        return;
    }
    ed->cl = ed->row_line[nr];
    int start = ed->row_start[nr], len = ed->row_len[nr];
    ed->cc = start + widget_text_index(&ed->w, ed->font, ed->lines[ed->cl] + start, len, ed->wanted_x);
}

static void set_pos_from_point(struct editor *ed, int px, int py)
{
    ensure_rows(ed);
    int r = ed->scroll + (py - 1) / LH(ed);
    if (r < 0) r = 0;
    if (r >= ed->nrows) r = ed->nrows - 1;
    ed->cl = ed->row_line[r];
    int rel = px - 1 - gutter_w(ed) - PAD + ed->scroll_x;
    ed->cc = ed->row_start[r] + widget_text_index(&ed->w, ed->font, ed->lines[ed->cl] + ed->row_start[r], ed->row_len[r],
                                                  rel < 0 ? 0 : rel);
}

static int editor_key(struct editor *ed, struct event *e)
{
    int ctrl = e->mods & WMOD_CTRL, shift = e->mods & WMOD_SHIFT;
    if (e->mods & WMOD_ALT)
        return 0;
    int vis = rows_visible(ed);
    int moved = 1;
    int bl = ed->cl, bc = ed->cc;
    if (ctrl) {
        switch (e->ch) {
        case 1: editor_select_all(&ed->w); return 1;
        case 3: editor_copy(&ed->w); return 1;
        case 24: editor_cut(&ed->w); return 1;
        case 22: editor_paste(&ed->w); return 1;
        case 26: if (!ed->readonly && editor_undo(&ed->w)) cursor_moved(ed); return 1;
        case 25: if (!ed->readonly && editor_redo(&ed->w)) cursor_moved(ed); return 1;
        }
    }
    if (ctrl && e->code == KEY_LEFT && ed->cc > 0)
        ed->cc = gui_word_left(ed->lines[ed->cl], ed->cc);
    else if (ctrl && e->code == KEY_RIGHT && ed->cc < llen(ed, ed->cl))
        ed->cc = gui_word_right(ed->lines[ed->cl], llen(ed, ed->cl), ed->cc);
    else if (e->code == KEY_HOME && !ctrl) {
        /* Home moves to the first character that is not a space, or to
         * column 0 when the cursor is already there. */
        int k = 0;
        while (ed->lines[ed->cl][k] == ' ' || ed->lines[ed->cl][k] == '\t')
            k++;
        ed->cc = ed->cc == k ? 0 : k;
    } else switch (e->code) {
    case KEY_LEFT: if (ed->cc > 0) ed->cc = gui_utf8_prev_boundary(ed->lines[ed->cl], ed->cc); else if (ed->cl > 0) { ed->cl--; ed->cc = llen(ed, ed->cl); } break;
    case KEY_RIGHT: if (ed->cc < llen(ed, ed->cl)) ed->cc = gui_utf8_next_boundary(ed->lines[ed->cl], llen(ed, ed->cl), ed->cc); else if (ed->cl + 1 < ed->nlines) { ed->cl++; ed->cc = 0; } break;
    case KEY_UP: move_vertical(ed, -1); break;
    case KEY_DOWN: move_vertical(ed, 1); break;
    case KEY_PAGEUP: move_vertical(ed, -vis); break;
    case KEY_PAGEDOWN: move_vertical(ed, vis); break;
    case KEY_HOME: ed->cl = 0; ed->cc = 0; break;
    case KEY_END: if (ctrl) ed->cl = ed->nlines - 1; ed->cc = llen(ed, ed->cl); break;
    default: moved = 0;
    }
    if (moved) {
        if (e->code != KEY_UP && e->code != KEY_DOWN && e->code != KEY_PAGEUP && e->code != KEY_PAGEDOWN)
            ed->wanted_x = -1;
        if (shift) {
            if (!ed->has_sel) { ed->al = bl; ed->ac = bc; ed->has_sel = 1; }
        } else {
            ed->has_sel = 0;
        }
        ed->merge = 0;
        cursor_moved(ed);
        return 1;
    }
    ed->wanted_x = -1;
    if (ed->readonly)
        return 0;
    if (ctrl && (e->code == KEY_BACKSPACE || e->code == KEY_DELETE) && !editor_has_selection(&ed->w)) {
        /* Ctrl+Backspace and Ctrl+Delete delete to the start of the word
         * before the cursor or to the end of the word after it. */
        if (e->code == KEY_BACKSPACE) {
            if (ed->cc > 0) delete_range(ed, ed->cl, gui_word_left(ed->lines[ed->cl], ed->cc), ed->cl, ed->cc);
            else if (ed->cl > 0) delete_range(ed, ed->cl - 1, llen(ed, ed->cl - 1), ed->cl, 0);
        } else {
            if (ed->cc < llen(ed, ed->cl)) delete_range(ed, ed->cl, ed->cc, ed->cl, gui_word_right(ed->lines[ed->cl], llen(ed, ed->cl), ed->cc));
            else if (ed->cl + 1 < ed->nlines) delete_range(ed, ed->cl, ed->cc, ed->cl + 1, 0);
        }
    } else if (e->code == KEY_DELETE) {
        if (!delete_selection(ed)) {
            if (ed->cc < llen(ed, ed->cl)) delete_range(ed, ed->cl, ed->cc, ed->cl, gui_utf8_next_boundary(ed->lines[ed->cl], llen(ed, ed->cl), ed->cc));
            else if (ed->cl + 1 < ed->nlines) delete_range(ed, ed->cl, ed->cc, ed->cl + 1, 0);
        }
    } else if (e->ch == '\b') {
        if (!delete_selection(ed)) {
            if (ed->cc > 0) delete_range(ed, ed->cl, gui_utf8_prev_boundary(ed->lines[ed->cl], ed->cc), ed->cl, ed->cc);
            else if (ed->cl > 0) delete_range(ed, ed->cl - 1, llen(ed, ed->cl - 1), ed->cl, 0);
        }
    } else if (e->ch == '\n') {
        /* The new line starts with the spaces and tabs that start the
         * current line before the cursor. */
        char indent[65] = "\n";
        int k = 0;
        while (k < 63 && k < ed->cc && (ed->lines[ed->cl][k] == ' ' || ed->lines[ed->cl][k] == '\t')) {
            indent[k + 1] = ed->lines[ed->cl][k];
            k++;
        }
        indent[k + 1] = '\0';
        group_begin(ed);
        delete_selection(ed);
        insert_text(ed, indent, k + 1, 0);
        group_end(ed);
    } else if (e->ch == '\t' || e->code == KEY_TAB) {
        int l0, c0, l1, c1;
        sel_range(ed, &l0, &c0, &l1, &c1);
        if (l1 > l0 && c1 == 0)
            l1--;
        if (shift || (editor_has_selection(&ed->w) && l1 > l0)) {
            indent_lines(ed, l0, l1, shift);
        } else {
            delete_selection(ed);
            insert_text(ed, "    ", 4, 0);
        }
    } else if (e->ch >= 32 && !ctrl) {
        char c[4];
        int n = gui_utf8_encode((uint32_t)e->ch, c);
        int had = delete_selection(ed);
        insert_text(ed, c, n, !had);
    } else {
        return 0;
    }
    cursor_moved(ed);
    return 1;
}

static void context_action(struct widget *w, enum edit_action a)
{
    struct editor *ed = (struct editor *)w;
    switch (a) {
    case EDIT_UNDO: if (!ed->readonly) editor_undo(w); break;
    case EDIT_REDO: if (!ed->readonly) editor_redo(w); break;
    case EDIT_CUT: editor_cut(w); break;
    case EDIT_COPY: editor_copy(w); break;
    case EDIT_PASTE: editor_paste(w); break;
    case EDIT_DELETE: editor_delete_selection(w); break;
    case EDIT_SELECT_ALL: editor_select_all(w); break;
    default: break;
    }
    cursor_moved(ed);
    widget_focus(w);
}

/* The text position under a point, the cursor unchanged. */
static void pos_at_point(struct editor *ed, int px, int py, int *l, int *c)
{
    int cl = ed->cl, cc = ed->cc;
    set_pos_from_point(ed, px, py);
    *l = ed->cl;
    *c = ed->cc;
    ed->cl = cl;
    ed->cc = cc;
}

/* inside_selection returns 1 when (l, c) is inside the selection. */
static int inside_selection(struct editor *ed, int l, int c)
{
    if (!editor_has_selection(&ed->w))
        return 0;
    int l0, c0, l1, c1;
    sel_range(ed, &l0, &c0, &l1, &c1);
    return (l > l0 || (l == l0 && c >= c0)) && (l < l1 || (l == l1 && c <= c1));
}

/* context_menu opens the context menu at (x, y).  A click outside the
 * selection first moves the cursor to the click. */
static void context_menu(struct editor *ed, int x, int y)
{
    int bl = ed->cl, bc = ed->cc;
    set_pos_from_point(ed, x, y);
    int l = ed->cl, c = ed->cc;
    ed->cl = bl;
    ed->cc = bc;
    if (!inside_selection(ed, l, c)) {
        ed->cl = l;
        ed->cc = c;
        ed->has_sel = 0;
    }
    widget_focus(&ed->w);
    cursor_moved(ed);
    int sel = editor_has_selection(&ed->w), rw = !ed->readonly;
    unsigned enabled = EDIT_BIT(EDIT_SELECT_ALL);
    if (rw && ed->nundo) enabled |= EDIT_BIT(EDIT_UNDO);
    if (rw && ed->nredo) enabled |= EDIT_BIT(EDIT_REDO);
    if (sel) enabled |= EDIT_BIT(EDIT_COPY);
    if (sel && rw) enabled |= EDIT_BIT(EDIT_CUT) | EDIT_BIT(EDIT_DELETE);
    if (rw) enabled |= EDIT_BIT(EDIT_PASTE);
    edit_menu_popup(&ed->w, &ed->context_menu, x, y, ~0u, enabled, context_action);
}

/* ---- drag and drop ---- */

static void drag_begin(struct editor *ed)
{
    char *text = editor_selection(&ed->w);
    if (!text)
        return;
    char label[48];
    size_t n = strcspn(text, "\n");
    snprintf(label, sizeof label, n > 32 || text[n] ? "%.32s…" : "%s", text);
    struct gui_drag_item item = { "text/plain", text, strlen(text) };
    int actions = GUI_DND_COPY | (ed->readonly ? 0 : GUI_DND_MOVE);
    if (widget_drag_start(&ed->w, &item, 1, actions, NULL, label) == 0) {
        sel_range(ed, &ed->dl0, &ed->dc0, &ed->dl1, &ed->dc1);
        ed->drag_out = 1;
        ed->drag_moved_here = 0;
        free(ed->drag_text);
        ed->drag_text = text;
    } else {
        free(text);
    }
}

/* A position after the deleted range [l0:c0, l1:c1) where it is now. */
static void pos_after_delete(int l0, int c0, int l1, int c1, int *l, int *c)
{
    if (*l < l1 || (*l == l1 && *c < c1))
        return;
    if (*l == l1) {
        *l = l0;
        *c = c0 + (*c - c1);
    } else {
        *l -= l1 - l0;
    }
}

/* The selection moved by a drop inside the same editor: one undo step. */
static void move_dragged(struct editor *ed, int l, int c)
{
    char *text = copy_range(ed, ed->dl0, ed->dc0, ed->dl1, ed->dc1);
    group_begin(ed);
    delete_range(ed, ed->dl0, ed->dc0, ed->dl1, ed->dc1);
    pos_after_delete(ed->dl0, ed->dc0, ed->dl1, ed->dc1, &l, &c);
    ed->cl = l;
    ed->cc = c;
    insert_text(ed, text, (int)strlen(text), 0);
    group_end(ed);
    free(text);
    ed->drag_moved_here = 1;
}

/* The dragged range still contains the dragged text. */
static int dragged_unchanged(struct editor *ed)
{
    if (!ed->drag_text || ed->dl1 >= ed->nlines || ed->dc0 > llen(ed, ed->dl0) || ed->dc1 > llen(ed, ed->dl1))
        return 0;
    char *now = copy_range(ed, ed->dl0, ed->dc0, ed->dl1, ed->dc1);
    int same = strcmp(now, ed->drag_text) == 0;
    free(now);
    return same;
}

static void set_dropping(struct editor *ed, int on, int l, int c)
{
    if (on != ed->dropping || l != ed->drop_l || c != ed->drop_c) {
        ed->dropping = on;
        ed->drop_l = l;
        ed->drop_c = c;
        widget_invalidate(&ed->w);
    }
}

/* The owner may take a drag first through "drag_motion" and "drop"
 * (sig_drag), as gedit does for files; otherwise text is inserted at the
 * drop caret, and the editor's own selection is moved there. */
static int editor_drag_event(struct editor *ed, struct event *e)
{
    struct widget *w = &ed->w;
    struct sig_drag sd = { -1, e->x, e->y, e->drag };
    switch (e->type) {
    case EV_DRAG_MOVE: {
        ed->drop_owner = widget_emit(w, "drag_motion", &sd);
        if (ed->drop_owner || ed->readonly || !widget_drag_offers("text/plain")) {
            set_dropping(ed, 0, 0, 0);
            return 1;
        }
        int l, c;
        pos_at_point(ed, e->x, e->y, &l, &c);
        int l0, c0, l1, c1;
        sel_range(ed, &l0, &c0, &l1, &c1);
        int in_drag = ed->drag_out && (l > l0 || (l == l0 && c > c0)) && (l < l1 || (l == l1 && c < c1));
        if (in_drag) {                  /* into the dragged text itself */
            set_dropping(ed, 0, 0, 0);
            return 1;
        }
        e->drag->accept_mime = "text/plain";
        e->drag->accept_actions = GUI_DND_COPY | GUI_DND_MOVE;
        e->drag->preferred = ed->drag_out ? GUI_DND_MOVE : GUI_DND_COPY;
        set_dropping(ed, 1, l, c);
        return 1;
    }
    case EV_DRAG_LEAVE:
        set_dropping(ed, 0, 0, 0);
        return 1;
    case EV_DROP:
        if (ed->drop_owner) {
            widget_emit(w, "drop", &sd);
            return 1;
        }
        if (!ed->dropping)
            return 1;
        int l = ed->drop_l, c = ed->drop_c;
        set_dropping(ed, 0, 0, 0);
        if (ed->drag_out && e->drag->action == GUI_DND_MOVE && dragged_unchanged(ed)) {
            move_dragged(ed, l, c);
        } else {
            ed->cl = l;
            ed->cc = c;
            ed->has_sel = 0;
            group_begin(ed);
            insert_text(ed, e->drag->data, (int)e->drag->len, 0);
            group_end(ed);
        }
        ed->has_sel = 0;
        widget_focus(w);
        cursor_moved(ed);
        return 1;
    case EV_DRAG_END:
        /* Text moved to another place loses its old copy. */
        if (ed->drag_out && e->drag->action == GUI_DND_MOVE && !ed->drag_moved_here && !ed->readonly &&
            dragged_unchanged(ed)) {
            group_begin(ed);
            delete_range(ed, ed->dl0, ed->dc0, ed->dl1, ed->dc1);
            group_end(ed);
            ed->cl = ed->dl0;
            ed->cc = ed->dc0;
            ed->has_sel = 0;
            cursor_moved(ed);
        }
        ed->drag_out = 0;
        free(ed->drag_text);
        ed->drag_text = NULL;
        return 1;
    default:
        return 0;
    }
}

/* The editor scrolled from the first row old: the rows and the line
 * numbers move by copy, and the track is repainted. A move up exposes
 * the rows at the bottom and the partly shown row above them. */
static void scrolled(struct editor *ed, int old)
{
    struct widget *w = &ed->w;
    int sb = theme_px(widget_theme(w), TM_SCROLLBAR), lh = LH(ed);
    struct rect r = { 1, 1, w->w - 2 - sb, w->h - 2 };
    int dy = (old - ed->scroll) * lh, rem = r.h % lh;
    widget_scroll_area(w, r, dy);
    if (dy < 0 && rem)
        widget_invalidate_rect(w, (struct rect){ r.x, r.y + r.h + dy - rem, r.w, rem });
    widget_invalidate_rect(w, (struct rect){ w->w - sb, 0, sb, w->h });
}

/* The scroll track at the right edge. */
static int track_event(struct editor *ed, struct event *e)
{
    struct widget *w = &ed->w;
    int sb = theme_px(widget_theme(w), TM_SCROLLBAR);
    ensure_rows(ed);
    int before = ed->scroll;
    struct rect track = { w->w - sb, 0, sb, w->h };
    if (!scroll_track_event(&ed->track, w, e, track, &ed->scroll, ed->nrows, rows_visible(ed)))
        return 0;
    if (ed->scroll != before)
        scrolled(ed, before);
    return 1;
}

static int editor_event(struct widget *w, struct event *e)
{
    struct editor *ed = (struct editor *)w;
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (e->button & 2) {
            context_menu(ed, e->x, e->y);
            return 1;
        }
        if (!(e->button & 1))
            return 0;
        if (track_event(ed, e))
            return 1;
        /* A double click selects the word, and a triple click the line. */
        int clicks = gui_click_count(&ed->clicks, e->x, e->y);
        /* A press inside the selection may drag it; the selection is
         * cleared only when the release shows that it was a click. */
        int pl, pc;
        pos_at_point(ed, e->x, e->y, &pl, &pc);
        if (clicks == 1 && inside_selection(ed, pl, pc)) {
            ed->drag_pending = 1;
            ed->press_x = e->x;
            ed->press_y = e->y;
            widget_capture(w);
            return 1;
        }
        set_pos_from_point(ed, e->x, e->y);
        ed->has_sel = 0;
        ed->wanted_x = -1;
        ed->merge = 0;
        if (clicks == 2)
            select_word(ed);
        else if (clicks == 3)
            select_line(ed);
        else
            widget_capture(w);
        cursor_moved(ed);
        return 1;
    }
    case EV_MOUSE_MOVE:
        if (track_event(ed, e))
            return 1;
        if (ed->drag_pending) {
            if ((e->button & 1) && widget_drag_moved(ed->press_x, ed->press_y, e->x, e->y)) {
                ed->drag_pending = 0;
                drag_begin(ed);
            }
            return 1;
        }
        if (e->button & 1) {
            int bl = ed->cl, bc = ed->cc;
            set_pos_from_point(ed, e->x, e->y);
            if (ed->cl != bl || ed->cc != bc) {
                if (!ed->has_sel) { ed->al = bl; ed->ac = bc; ed->has_sel = 1; }
                cursor_moved(ed);
            }
            return 1;
        }
        return 0;
    case EV_MOUSE_WHEEL: {
        ensure_rows(ed);
        int old = ed->scroll;
        if (scroll_set(&ed->scroll, ed->scroll + 3 * e->button, ed->nrows, rows_visible(ed)))
            scrolled(ed, old);
        return 1;
    }
    case EV_KEY_DOWN:
        return editor_key(ed, e);
    case EV_TEXT:
        if (!ed->readonly && e->text && *e->text) {
            ed->preedit[0] = '\0';
            delete_selection(ed);
            insert_text(ed, e->text, (int)strlen(e->text), 0);
            cursor_moved(ed);
        }
        return 1;
    case EV_PREEDIT:
        strlcpy(ed->preedit, e->text ? e->text : "", sizeof ed->preedit);
        widget_invalidate(w);
        return 1;
    case EV_TEXT_DELETE:
        if (!ed->readonly && e->before >= 0 && e->after >= 0) {
            int len = llen(ed, ed->cl);
            int a = ed->cc - e->before, b = ed->cc + e->after;
            if (a < 0) a = 0;
            if (b > len) b = len;
            while (a > 0 && ((unsigned char)ed->lines[ed->cl][a] & 0xc0) == 0x80) a--;
            while (b < len && ((unsigned char)ed->lines[ed->cl][b] & 0xc0) == 0x80) b++;
            delete_range(ed, ed->cl, a, ed->cl, b);
            cursor_moved(ed);
        }
        return 1;
    case EV_MOUSE_UP:
        if (track_event(ed, e))
            return 1;
        if (ed->drag_pending) {         /* a click inside the selection */
            ed->drag_pending = 0;
            set_pos_from_point(ed, ed->press_x, ed->press_y);
            ed->has_sel = 0;
            ed->wanted_x = -1;
            cursor_moved(ed);
            return 1;
        }
        return 0;
    case EV_DRAG_MOVE: case EV_DRAG_LEAVE: case EV_DROP: case EV_DRAG_END:
        return editor_drag_event(ed, e);
    case EV_FOCUS_IN: case EV_FOCUS_OUT:
        widget_invalidate(w);
        return 1;
    default:
        return 0;
    }
}

static void editor_measure(struct widget *w, struct size_hint *h)
{
    h->pref_w = 300;
    h->pref_h = 200;
    h->min_w = 60;
    h->min_h = LH((struct editor *)w) + 2;
}

static void editor_destroy(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    for (int i = 0; i < ed->nlines; i++)
        free(ed->lines[i]);
    free(ed->lines);
    clear_ops(&ed->undo, &ed->nundo);
    clear_ops(&ed->redo, &ed->nredo);
    free(ed->row_line);
    free(ed->first_row);
    free(ed->hl_state);
    free(ed->row_start);
    free(ed->row_len);
    free(ed->drag_text);
    if (ed->font)
        gfx_font_free(ed->font);
}

const struct widget_class editor_class = { "editor", sizeof(struct editor), editor_measure, NULL, editor_paint, editor_event, editor_destroy };
