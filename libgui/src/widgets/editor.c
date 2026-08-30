/* Text editor widget: an array of lines, a cursor with a selection
 * anchor, an undo and redo stack of insert and delete operations,
 * optional word wrap and line numbers, search, a highlighter and the
 * clipboard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "editor_internal.h"
void scrollbar_paint_track(struct painter *p, int x, int y, int w, int h, int value, int max, int page, int vertical);

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

void editor_changed(struct editor *ed)
{
    ed->rows_dirty = 1;
    ed->modified = 1;
    widget_invalidate(&ed->w);
    struct sig_change c = { ed->cl, NULL };
    widget_emit(&ed->w, "changed", &c);
}

/* Raw insert of text (may contain newlines) at (l, c); *el, *ec receive
 * the end position. */
static void raw_insert(struct editor *ed, int l, int c, const char *text, int n, int *el, int *ec)
{
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

/* Raw delete of the range; returns the removed text. */
static char *raw_delete(struct editor *ed, int l0, int c0, int l1, int c1)
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
    if (mergeable && ed->merge && ed->nundo) {
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
    struct op op = { 0, ed->cl, ed->cc, malloc((size_t)n + 1), n };
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
    struct op op = { 1, l0, c0, text, (int)strlen(text) };
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
        struct op inv = { 1, op->l, op->c, op->text, op->len };
        push(to, nto, inv);
        ed->cl = op->l;
        ed->cc = op->c;
    } else {
        int el, ec;
        raw_insert(ed, op->l, op->c, op->text, op->len, &el, &ec);
        struct op inv = { 0, op->l, op->c, op->text, op->len };
        push(to, nto, inv);
        ed->cl = el;
        ed->cc = ec;
    }
    ed->has_sel = 0;
    ed->merge = 0;
    editor_changed(ed);
}

int editor_undo(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    if (!ed->nundo)
        return 0;
    struct op op = ed->undo[--ed->nundo];
    apply(ed, &op, &ed->redo, &ed->nredo);
    return 1;
}

int editor_redo(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    if (!ed->nredo)
        return 0;
    struct op op = ed->redo[--ed->nredo];
    apply(ed, &op, &ed->undo, &ed->nundo);
    return 1;
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

/* ---- visual rows ---- */

static void add_row(struct editor *ed, int line, int start, int len)
{
    if ((ed->nrows & 127) == 0) {
        ed->row_line = realloc(ed->row_line, (size_t)(ed->nrows + 128) * sizeof *ed->row_line);
        ed->row_start = realloc(ed->row_start, (size_t)(ed->nrows + 128) * sizeof *ed->row_start);
        ed->row_len = realloc(ed->row_len, (size_t)(ed->nrows + 128) * sizeof *ed->row_len);
    }
    ed->row_line[ed->nrows] = line;
    ed->row_start[ed->nrows] = start;
    ed->row_len[ed->nrows] = len;
    ed->nrows++;
}

static int gutter_w(struct editor *ed)
{
    if (!ed->numbers)
        return 0;
    int digits = 1;
    for (int n = ed->nlines; n >= 10; n /= 10)
        digits++;
    return digits * widget_theme(&ed->w)->font->advance['0'] + 10;
}

static int text_w(struct editor *ed)
{
    int sb = theme_px(widget_theme(&ed->w), TM_SCROLLBAR);
    return ed->w.w - 2 - gutter_w(ed) - 2 * PAD - sb;
}

static void build_rows(struct editor *ed)
{
    const struct font *f = widget_theme(&ed->w)->font;
    int avail = text_w(ed);
    ed->nrows = 0;
    for (int l = 0; l < ed->nlines; l++) {
        const char *s = ed->lines[l];
        int len = (int)strlen(s);
        if (!ed->wrap || avail < 40) {
            add_row(ed, l, 0, len);
            continue;
        }
        int start = 0;
        while (start < len) {
            int fit = gfx_text_index_font(f, s + start, len - start, avail);
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
        if (len == 0)
            add_row(ed, l, 0, 0);
    }
    ed->rows_dirty = 0;
    ed->rows_width = avail;
}

static int row_of(struct editor *ed, int l, int c)
{
    int last = 0;
    for (int r = 0; r < ed->nrows; r++) {
        if (ed->row_line[r] != l)
            continue;
        last = r;
        if (c >= ed->row_start[r] && c < ed->row_start[r] + ed->row_len[r])
            return r;
    }
    return last;
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
        const struct font *f = widget_theme(&ed->w)->font;
        int cx = gfx_text_width_font(f, ed->lines[ed->cl], ed->cc);
        int avail = text_w(ed);
        if (cx < ed->scroll_x) ed->scroll_x = cx;
        if (cx > ed->scroll_x + avail - 1) ed->scroll_x = cx - avail + 1;
    } else {
        ed->scroll_x = 0;
    }
}

void cursor_moved(struct editor *ed)
{
    scroll_to_cursor(ed);
    widget_invalidate(&ed->w);
    struct sig_change c = { ed->cl, NULL };
    widget_emit(&ed->w, "cursor", &c);
}

/* ---- painting ---- */

static uint32_t class_color(const struct theme *t, int cls)
{
    switch (cls) {
    case HL_KEYWORD: return 0x001040c0;
    case HL_STRING: return 0x00b03020;
    case HL_COMMENT: return 0x00308030;
    case HL_NUMBER: return 0x00901090;
    case HL_PREPROC: return 0x00806020;
    default: return t->color[TC_TEXT];
    }
}

static void editor_paint(struct widget *w, struct painter *p)
{
    struct editor *ed = (struct editor *)w;
    const struct theme *t = p->theme;
    ensure_rows(ed);
    int lh = LH(ed), vis = rows_visible(ed), gw = gutter_w(ed);
    int sb = theme_px(t, TM_SCROLLBAR);
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[w->focused ? TC_ACCENT : TC_BORDER]);
    if (gw) {
        painter_fill(p, 1, 1, gw, w->h - 2, t->color[TC_WINDOW]);
        painter_line(p, gw, 1, gw, w->h - 2, t->color[TC_BORDER]);
    }
    int l0, c0, l1, c1;
    sel_range(ed, &l0, &c0, &l1, &c1);
    /* Highlighter state up to the first visible line. */
    int state = 0;
    unsigned char *classes = NULL;
    int first_line = ed->scroll < ed->nrows ? ed->row_line[ed->scroll] : 0;
    if (ed->hl) {
        for (int l = 0; l < first_line; l++) {
            int len = llen(ed, l);
            classes = realloc(classes, (size_t)len + 1);
            ed->hl(ed->lines[l], len, classes, &state, ed->hl_arg);
        }
    }
    int last_hl_line = -1;
    painter_push(p, 1 + gw, 1, w->w - 2 - gw - sb, w->h - 2);
    for (int i = 0; i <= vis && ed->scroll + i < ed->nrows; i++) {
        int r = ed->scroll + i, l = ed->row_line[r], start = ed->row_start[r], len = ed->row_len[r];
        const char *s = ed->lines[l];
        int y = i * lh, ty = y + 1;
        int x = PAD - ed->scroll_x;
        if (ed->hl && l != last_hl_line) {
            int ll = llen(ed, l);
            classes = realloc(classes, (size_t)ll + 1);
            ed->hl(s, ll, classes, &state, ed->hl_arg);
            last_hl_line = l;
        }
        /* Selection background on this row. */
        if ((l > l0 || (l == l0 && start + len >= c0)) && (l < l1 || (l == l1 && start <= c1)) && !(l0 == l1 && c0 == c1)) {
            int sa = l == l0 && c0 > start ? c0 : start;
            int sb2 = l == l1 && c1 < start + len ? c1 : start + len;
            int x0 = x + gfx_text_width_font(t->font, s + start, sa - start);
            int x1 = x + gfx_text_width_font(t->font, s + start, sb2 - start);
            if (l < l1 && sb2 == start + len)
                x1 += t->font->advance[' '];
            painter_fill(p, x0, y, x1 - x0, lh, t->color[TC_HIGHLIGHT]);
        }
        /* Text in runs of one class. */
        int j = start;
        char buf[512];
        while (j < start + len) {
            int cls = ed->hl ? classes[j] : 0;
            int k = j;
            while (k < start + len && (ed->hl ? classes[k] : 0) == cls && k - j < (int)sizeof buf - 1)
                k++;
            memcpy(buf, s + j, (size_t)(k - j));
            buf[k - j] = '\0';
            painter_text(p, x, ty, buf, class_color(t, cls));
            x += gfx_text_width_font(t->font, buf, -1);
            j = k;
        }
        if (w->focused && !ed->readonly && l == ed->cl && ed->cc >= start && (ed->cc < start + len || (ed->cc == start + len && (r + 1 >= ed->nrows || ed->row_line[r + 1] != l)))) {
            int cx = PAD - ed->scroll_x + gfx_text_width_font(t->font, s + start, ed->cc - start);
            painter_line(p, cx, y, cx, y + lh - 1, t->color[TC_TEXT]);
        }
    }
    painter_pop(p);
    free(classes);
    if (gw) {
        painter_push(p, 1, 1, gw - 1, w->h - 2);
        for (int i = 0; i <= vis && ed->scroll + i < ed->nrows; i++) {
            int r = ed->scroll + i;
            if (ed->row_start[r] != 0)
                continue;
            char num[16];
            snprintf(num, sizeof num, "%d", ed->row_line[r] + 1);
            int tw = gfx_text_width_font(t->font, num, -1);
            painter_text(p, gw - 6 - tw, i * lh + 1, num, t->color[TC_TEXT_DISABLED]);
        }
        painter_pop(p);
    }
    scrollbar_paint_track(p, w->w - sb, 0, sb, w->h, ed->scroll, ed->nrows, vis, 1);
}

/* ---- editing keys ---- */

static void move_vertical(struct editor *ed, int rows)
{
    ensure_rows(ed);
    const struct font *f = widget_theme(&ed->w)->font;
    int r = row_of(ed, ed->cl, ed->cc);
    if (ed->wanted_x < 0)
        ed->wanted_x = gfx_text_width_font(f, ed->lines[ed->cl] + ed->row_start[r], ed->cc - ed->row_start[r]);
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
    ed->cc = start + gfx_text_index_font(f, ed->lines[ed->cl] + start, len, ed->wanted_x);
}

static void set_pos_from_point(struct editor *ed, int px, int py)
{
    ensure_rows(ed);
    const struct font *f = widget_theme(&ed->w)->font;
    int r = ed->scroll + (py - 1) / LH(ed);
    if (r < 0) r = 0;
    if (r >= ed->nrows) r = ed->nrows - 1;
    ed->cl = ed->row_line[r];
    int rel = px - 1 - gutter_w(ed) - PAD + ed->scroll_x;
    ed->cc = ed->row_start[r] + gfx_text_index_font(f, ed->lines[ed->cl] + ed->row_start[r], ed->row_len[r], rel < 0 ? 0 : rel);
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
        case 1: ed->al = 0; ed->ac = 0; ed->has_sel = 1; ed->cl = ed->nlines - 1; ed->cc = llen(ed, ed->cl); cursor_moved(ed); return 1;
        case 3: case 24: {
            int l0, c0, l1, c1;
            sel_range(ed, &l0, &c0, &l1, &c1);
            if (l0 != l1 || c0 != c1) {
                char *text = raw_delete(ed, l0, c0, l1, c1);
                int el, ec;
                raw_insert(ed, l0, c0, text, (int)strlen(text), &el, &ec);
                gui_clipboard_set(text, (int)strlen(text));
                free(text);
                if (e->ch == 24 && !ed->readonly)
                    delete_selection(ed);
            }
            return 1;
        }
        case 22: {
            if (ed->readonly)
                return 1;
            char *tmp = malloc(WSRV_CLIP_MAX);
            int n = tmp ? gui_clipboard_get(tmp, WSRV_CLIP_MAX) : -1;
            if (n > 0) {
                delete_selection(ed);
                insert_text(ed, tmp, n, 0);
                cursor_moved(ed);
            }
            free(tmp);
            return 1;
        }
        case 26: if (!ed->readonly && editor_undo(&ed->w)) cursor_moved(ed); return 1;
        case 25: if (!ed->readonly && editor_redo(&ed->w)) cursor_moved(ed); return 1;
        }
    }
    switch (e->code) {
    case 0xcb: if (ed->cc > 0) ed->cc--; else if (ed->cl > 0) { ed->cl--; ed->cc = llen(ed, ed->cl); } break;
    case 0xcd: if (ed->cc < llen(ed, ed->cl)) ed->cc++; else if (ed->cl + 1 < ed->nlines) { ed->cl++; ed->cc = 0; } break;
    case 0xc8: move_vertical(ed, -1); break;
    case 0xd0: move_vertical(ed, 1); break;
    case 0xc9: move_vertical(ed, -vis); break;
    case 0xd1: move_vertical(ed, vis); break;
    case 0xc7: if (ctrl) { ed->cl = 0; } ed->cc = 0; break;
    case 0xcf: if (ctrl) ed->cl = ed->nlines - 1; ed->cc = llen(ed, ed->cl); break;
    default: moved = 0;
    }
    if (moved) {
        if (e->code != 0xc8 && e->code != 0xd0 && e->code != 0xc9 && e->code != 0xd1)
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
    if (e->code == 0xd3) {
        if (!delete_selection(ed)) {
            if (ed->cc < llen(ed, ed->cl)) delete_range(ed, ed->cl, ed->cc, ed->cl, ed->cc + 1);
            else if (ed->cl + 1 < ed->nlines) delete_range(ed, ed->cl, ed->cc, ed->cl + 1, 0);
        }
    } else if (e->ch == '\b') {
        if (!delete_selection(ed)) {
            if (ed->cc > 0) delete_range(ed, ed->cl, ed->cc - 1, ed->cl, ed->cc);
            else if (ed->cl > 0) delete_range(ed, ed->cl - 1, llen(ed, ed->cl - 1), ed->cl, 0);
        }
    } else if (e->ch == '\n') {
        delete_selection(ed);
        insert_text(ed, "\n", 1, 0);
    } else if (e->ch == '\t') {
        delete_selection(ed);
        insert_text(ed, "    ", 4, 0);
    } else if (e->ch >= 32 && e->ch < 127 && !ctrl) {
        char c = (char)e->ch;
        int had = delete_selection(ed);
        insert_text(ed, &c, 1, !had);
    } else {
        return 0;
    }
    cursor_moved(ed);
    return 1;
}

static int editor_event(struct widget *w, struct event *e)
{
    struct editor *ed = (struct editor *)w;
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (!(e->button & 1))
            return 0;
        int sb = theme_px(widget_theme(w), TM_SCROLLBAR);
        if (e->x >= w->w - sb) {
            ensure_rows(ed);
            int vis = rows_visible(ed);
            int mid = 1 + (w->h - 2) * (ed->scroll + vis / 2) / (ed->nrows ? ed->nrows : 1);
            int top = ed->nrows - vis;
            ed->scroll += e->y < mid ? -vis : vis;
            if (ed->scroll > top) ed->scroll = top;
            if (ed->scroll < 0) ed->scroll = 0;
            widget_invalidate(w);
            return 1;
        }
        set_pos_from_point(ed, e->x, e->y);
        ed->has_sel = 0;
        ed->wanted_x = -1;
        ed->merge = 0;
        widget_capture(w);
        cursor_moved(ed);
        return 1;
    }
    case EV_MOUSE_MOVE:
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
        int top = ed->nrows - rows_visible(ed);
        ed->scroll += 3 * e->button;
        if (ed->scroll > top) ed->scroll = top;
        if (ed->scroll < 0) ed->scroll = 0;
        widget_invalidate(w);
        return 1;
    }
    case EV_KEY_DOWN:
        return editor_key(ed, e);
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
    free(ed->row_start);
    free(ed->row_len);
}

const struct widget_class editor_class = { "editor", sizeof(struct editor), editor_measure, NULL, editor_paint, editor_event, editor_destroy };
