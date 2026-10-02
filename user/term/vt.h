#pragma once
/* Terminal emulation without drawing: a grid of cells with colours and
 * attributes, a scrollback ring, an alternate screen, a scrolling
 * region and a parser for the ECMA-48 and xterm subset listed in
 * docs/design/terminal.md. The window (term.c) feeds bytes from the
 * pseudo terminal and paints the lines it gets back. */
#include <stdint.h>
#include <stddef.h>

#define VA_BOLD 1
#define VA_FAINT 2
#define VA_ITALIC 4
#define VA_UNDERLINE 8
#define VA_REVERSE 16
#define VA_STRIKE 32

/* Colours are 0x00rrggbb; this bit marks the default foreground or
 * background, which the window paints with its own colours. */
#define VC_DEFAULT 0x01000000u

/* The right half of a wide character has this value in cp. */
#define VC_WIDE_TAIL 0x110000u

struct vcell {
    uint32_t cp;                /* code point, ' ' when empty */
    uint32_t mark;              /* a combining mark drawn over cp, or 0 */
    uint32_t fg, bg;
    uint8_t attr;               /* VA_* */
};

struct vline {
    struct vcell *cells;
    int len;                    /* cells kept after trimming blanks */
};

struct vt {
    int cols, rows;
    struct vcell *cells;        /* rows * cols, the visible screen */
    struct vcell *main, *alt;   /* the two screens; cells is one of them */
    int cx, cy;
    int wrap_pending;           /* a character was written in the last column */
    int top, bot;               /* scrolling region, inclusive rows */
    /* current rendition */
    uint32_t fg, bg;
    int fg_index, bg_index;     /* palette index when a 16 colour SGR set it, else -1 */
    uint8_t attr;
    /* saved cursor (DECSC) */
    int saved_cx, saved_cy, saved_fg_index, saved_bg_index;
    uint32_t saved_fg, saved_bg;
    uint8_t saved_attr;
    int saved_valid;
    /* modes */
    unsigned cursor_visible : 1, autowrap : 1, origin : 1, insert : 1, app_cursor : 1, bracketed_paste : 1;
    unsigned newline : 1;       /* LNM: LF also returns to column 0 (the tty has no ONLCR), default on */
    unsigned bell : 1;          /* set by BEL, cleared by the window */
    int dirty;                  /* anything changed since the window cleared it */
    /* parser */
    int state;
    int params[16], nparams;
    int priv;                   /* the '?' of a private CSI sequence */
    int intermediate;
    char osc[128];
    int osc_len;
    uint32_t utf8_cp;
    int utf8_need;
    char title[64];
    int title_changed;
    char reply[64];             /* answers to DSR and DA for the pty */
    int reply_len;
    /* scrollback ring: oldest line at head */
    struct vline *sb;
    int sb_cap, sb_count, sb_head;
};

struct vt *vt_create(int cols, int rows, int scrollback);
void vt_free(struct vt *v);
void vt_resize(struct vt *v, int cols, int rows);
void vt_feed(struct vt *v, const char *data, size_t n);
/* Lines are numbered from the oldest scrollback line (0) through the
 * screen rows (sb_count .. sb_count + rows - 1). */
int vt_total_lines(const struct vt *v);
const struct vcell *vt_line(const struct vt *v, int index, int *len);
void vt_clear_scrollback(struct vt *v);
/* The xterm palette entry 0..15. */
uint32_t vt_palette(int index);
/* Move the pending reply into buf; returns its length. */
int vt_take_reply(struct vt *v, char *buf, int size);
