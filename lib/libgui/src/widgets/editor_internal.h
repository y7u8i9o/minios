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
    int nrows, rows_cap, rows_dirty, rows_width;
    int *first_row;             /* the first row of each line, nlines + 1 entries */
    /* The edit since the last row update: the lines edit_line to
     * edit_line + edit_old - 1 became edit_new lines. edit_old is -1
     * without an edit. A second edit before the update sets rows_dirty. */
    int edit_line, edit_old, edit_new;
    /* hl_state[l] is the state of the highlighter at the start of line l.
     * The entries 0 to hl_valid are valid. */
    int *hl_state, hl_valid;
    int wanted_x;               /* column pixel to retain on vertical moves */
    /* The row of the cursor and the presence of a selection at the last
     * cursor_moved. caret_row is -1 before the first move. */
    int caret_row, sel_shown;
    char preedit[WSRV_TITLE_MAX];
    struct font *font;          /* own font (editor_set_font), NULL for the theme's */
    int group, next_group;      /* group is the undo group of new operations, or 0. */
    struct gui_clicks clicks;   /* the left clicks, for word and line selection */
    struct scroll_track track;
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
/* Records that the lines l to l + old_n - 1 became new_n lines. */
void lines_changed(struct editor *ed, int l, int old_n, int new_n);
int llen(const struct editor *ed, int l);
void clear_ops(struct op **stack, int *n);
void cursor_moved(struct editor *ed);
void editor_changed(struct editor *ed);
