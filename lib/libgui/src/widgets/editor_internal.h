#pragma once
/* Shared between editor.c (editing, painting, events) and editor_api.c. */
#include <gui/app.h>

struct op {
    int kind;                   /* 0 insert, 1 delete */
    int l, c;
    char *text;
    int len;
    int group;                  /* Operations with the same nonzero group are undone together. */
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
    int wanted_x;               /* column pixel to retain on vertical moves */
    char preedit[WSRV_TITLE_MAX];
    struct font *font;          /* own font (editor_set_font), NULL for the theme's */
    int group, next_group;      /* group is the undo group of new operations, or 0. */
    long last_click_ms;         /* The time, position and count of the last left click. */
    int click_x, click_y, clicks;
    struct widget *context_menu;
    /* Drag and drop: a press inside the selection that may become a drag,
     * the dragged range and text while the drag runs, and the drop caret
     * while a drag of text is over the editor. */
    int drag_pending, press_x, press_y;
    int drag_out, drag_moved_here;
    int dl0, dc0, dl1, dc1;
    char *drag_text;
    int dropping, drop_l, drop_c;
    int drop_owner;             /* a "drag_motion" handler took the drag */
};

static inline const struct font *ed_font(const struct editor *ed)
{
    return ed->font ? ed->font : widget_theme(&ed->w)->font;
}

#define LH(ed) (ed_font(ed)->height + 2)
#define PAD 4

void lines_insert(struct editor *ed, int at, char *line);
int llen(const struct editor *ed, int l);
void clear_ops(struct op **stack, int *n);
void cursor_moved(struct editor *ed);
void editor_changed(struct editor *ed);
