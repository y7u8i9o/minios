/* Editor widget API: construction, text access, options, search. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "editor_internal.h"

struct widget *editor_new(struct widget *parent)
{
    struct widget *w = widget_new(&editor_class, parent);
    if (!w)
        return NULL;
    struct editor *ed = (struct editor *)w;
    lines_insert(ed, 0, strdup(""));
    ed->rows_dirty = 1;
    ed->wanted_x = -1;
    w->focusable = 1;
    widget_set_stretch(w, 1, 1);
    return w;
}

void editor_set_text(struct widget *w, const char *text)
{
    struct editor *ed = (struct editor *)w;
    for (int i = 0; i < ed->nlines; i++)
        free(ed->lines[i]);
    ed->nlines = 0;
    const char *s = text;
    for (;;) {
        const char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);
        char *line = malloc(n + 1);
        memcpy(line, s, n);
        line[n] = '\0';
        lines_insert(ed, ed->nlines, line);
        if (!nl)
            break;
        s = nl + 1;
    }
    clear_ops(&ed->undo, &ed->nundo);
    clear_ops(&ed->redo, &ed->nredo);
    ed->cl = ed->cc = 0;
    ed->has_sel = 0;
    ed->scroll = ed->scroll_x = 0;
    ed->rows_dirty = 1;
    ed->modified = 0;
    widget_invalidate(w);
}

char *editor_text(struct widget *w)
{
    struct editor *ed = (struct editor *)w;
    size_t total = 1;
    for (int i = 0; i < ed->nlines; i++)
        total += strlen(ed->lines[i]) + 1;
    char *out = malloc(total), *p = out;
    for (int i = 0; i < ed->nlines; i++) {
        size_t n = strlen(ed->lines[i]);
        memcpy(p, ed->lines[i], n);
        p += n;
        if (i + 1 < ed->nlines)
            *p++ = '\n';
    }
    *p = '\0';
    return out;
}

int editor_line_count(const struct widget *w) { return ((const struct editor *)w)->nlines; }
const char *editor_line(const struct widget *w, int i)
{
    const struct editor *ed = (const struct editor *)w;
    return i >= 0 && i < ed->nlines ? ed->lines[i] : NULL;
}

void editor_set_wrap(struct widget *w, int on)
{
    struct editor *ed = (struct editor *)w;
    ed->wrap = !!on;
    ed->rows_dirty = 1;
    ed->scroll_x = 0;
    widget_invalidate(w);
}

void editor_set_line_numbers(struct widget *w, int on)
{
    struct editor *ed = (struct editor *)w;
    ed->numbers = !!on;
    ed->rows_dirty = 1;
    widget_invalidate(w);
}

void editor_set_readonly(struct widget *w, int on) { ((struct editor *)w)->readonly = !!on; widget_invalidate(w); }

void editor_set_highlighter(struct widget *w, highlight_fn fn, void *arg)
{
    struct editor *ed = (struct editor *)w;
    ed->hl = fn;
    ed->hl_arg = arg;
    widget_invalidate(w);
}

void editor_goto(struct widget *w, int line, int col)
{
    struct editor *ed = (struct editor *)w;
    if (line < 0) line = 0;
    if (line >= ed->nlines) line = ed->nlines - 1;
    int len = llen(ed, line);
    ed->cl = line;
    ed->cc = col < 0 ? 0 : col > len ? len : col;
    ed->has_sel = 0;
    cursor_moved(ed);
}

void editor_cursor(const struct widget *w, int *line, int *col)
{
    const struct editor *ed = (const struct editor *)w;
    if (line) *line = ed->cl;
    if (col) *col = ed->cc;
}

int editor_find(struct widget *w, const char *needle, int forward)
{
    struct editor *ed = (struct editor *)w;
    int n = (int)strlen(needle);
    if (!n)
        return 0;
    int l = ed->cl, c = forward ? ed->cc + (ed->has_sel ? 0 : 0) : ed->cc;
    if (forward && ed->has_sel)
        c = ed->cc;
    for (int pass = 0; pass <= ed->nlines; pass++) {
        const char *s = ed->lines[l];
        int len = (int)strlen(s);
        if (forward) {
            for (int i = c; i + n <= len; i++)
                if (memcmp(s + i, needle, (size_t)n) == 0) {
                    ed->al = l; ed->ac = i; ed->has_sel = 1;
                    ed->cl = l; ed->cc = i + n;
                    cursor_moved(ed);
                    return 1;
                }
            l = (l + 1) % ed->nlines;
            c = 0;
        } else {
            int start = pass == 0 ? (ed->has_sel ? (ed->al < ed->cl || (ed->al == ed->cl && ed->ac < ed->cc) ? ed->ac : ed->cc) : c) - 1 : len - n;
            for (int i = start; i >= 0; i--)
                if (i + n <= len && memcmp(s + i, needle, (size_t)n) == 0) {
                    ed->al = l; ed->ac = i; ed->has_sel = 1;
                    ed->cl = l; ed->cc = i + n;
                    cursor_moved(ed);
                    return 1;
                }
            l = (l + ed->nlines - 1) % ed->nlines;
        }
    }
    return 0;
}

int editor_modified(const struct widget *w) { return ((const struct editor *)w)->modified; }
void editor_set_modified(struct widget *w, int m) { ((struct editor *)w)->modified = !!m; }
