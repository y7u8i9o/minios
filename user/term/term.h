#pragma once
/* The terminal window: one struct tab per page, shared by term.c (the
 * window, painting, keys, the pseudo terminal) and select.c (the mouse
 * selection and the clipboard). */
#include <gui/app.h>
#include "vt.h"

#define DEFAULT_BG 0x001e1e1e
#define DEFAULT_FG 0x00d4d4d4
#define SELECTION_BG 0x00264f78
#define CURSOR 0x00d4d4d4
#define PAD_X 4
#define PAD_Y 3
#define SCROLLBACK 2000
#define MAX_TABS 12
#define MIN_COLS 20
#define MIN_ROWS 5
#define FONT_PATH "/etc/fonts/DejaVuSansMono.ttf"
#define DEFAULT_PX 13

void scrollbar_paint_track(struct painter *p, int x, int y, int w, int h, int value, int max, int page, int vertical);

struct tab {
    struct vt *vt;
    int master;
    pid_t pid;
    struct widget *page, *canvas;
    struct watch *watch;
    int view;                   /* lines scrolled back, 0 shows the live screen */
    /* selection in line numbers of vt_line; a before b */
    int sel_valid, sel_ay, sel_ax, sel_by, sel_bx;
    int selecting, sel_mode;    /* 0 characters, 1 words, 2 lines */
    int anchor_y, anchor_x;
    long click_ms;
    int clicks;
    char title[64];
    char preedit[256];          /* the composition of an input method at the cursor */
};


extern int cell_w, cell_h;

/* term.c */
void set_view(struct tab *t, int v);
/* select.c */
int selected(const struct tab *t, int line, int col);
void clear_selection(struct tab *t);
void cell_at(struct tab *t, int px, int py, int *line, int *col);
void extend_selection(struct tab *t, int line, int col);
void write_all(int fd, const char *s, size_t n);
void copy_selection(struct tab *t);
void paste_clipboard(struct tab *t);
