#pragma once
/* Shared between editor.c (editing, painting, events) and editor_api.c. */
#include <gui/app.h>

struct op {
    int kind;                   /* 0 insert, 1 delete */
    int l, c;
    char *text;
    int len;
};

struct editor {
    struct widget w;
    char **lines;
    int nlines;
    int cl, cc;                 /* cursor */
    int al, ac, has_sel;        /* selection anchor */
    int scroll, scroll_x;       /* first visible row, horizontal pixels */
    int wrap, numbers, readonly, modified;
    struct op *undo, *redo;
    int nundo, nredo;
    int merge;                  /* the last undo op may absorb typed characters */
    highlight_fn hl;
    void *hl_arg;
    /* visual rows */
    int *row_line, *row_start, *row_len;
    int nrows, rows_dirty, rows_width;
    int wanted_x;               /* column pixel to keep on vertical moves */
};

#define LH(ed) (widget_theme(&(ed)->w)->font->height + 2)
#define PAD 4

#define LH(ed) (widget_theme(&(ed)->w)->font->height + 2)

void lines_insert(struct editor *ed, int at, char *line);
int llen(const struct editor *ed, int l);
void clear_ops(struct op **stack, int *n);
void cursor_moved(struct editor *ed);
void editor_changed(struct editor *ed);
