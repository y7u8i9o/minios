/* The mouse selection of a terminal tab and the clipboard: selection
 * coordinates are line numbers of vt_line, so a selection survives
 * scrolling; copy joins the selected cells as UTF-8 text. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gui/utf8.h>
#include "term.h"

int selected(const struct tab *t, int line, int col)
{
    if (!t->sel_valid)
        return 0;
    if (line < t->sel_ay || line > t->sel_by)
        return 0;
    if (line == t->sel_ay && col < t->sel_ax)
        return 0;
    if (line == t->sel_by && col > t->sel_bx)
        return 0;
    return 1;
}

void clear_selection(struct tab *t)
{
    if (t->sel_valid) {
        t->sel_valid = 0;
        widget_invalidate(t->canvas);
    }
}

void cell_at(struct tab *t, int px, int py, int *line, int *col)
{
    int c = (px - PAD_X) / cell_w, r = (py - PAD_Y) / cell_h;
    if (px < PAD_X) c = 0;
    if (py < PAD_Y) r = 0;
    if (c > t->vt->cols - 1) c = t->vt->cols - 1;
    if (r > t->vt->rows - 1) r = t->vt->rows - 1;
    *line = vt_total_lines(t->vt) - t->vt->rows - t->view + r;
    *col = c;
}

static int word_char(uint32_t cp) { return cp > ' ' && cp != 0xfffd; }

void extend_selection(struct tab *t, int line, int col)
{
    int ay = t->anchor_y, ax = t->anchor_x, by = line, bx = col;
    if (by < ay || (by == ay && bx < ax)) {
        int y = ay, x = ax;
        ay = by; ax = bx;
        by = y; bx = x;
    }
    if (t->sel_mode == 1) {
        int len;
        const struct vcell *l = vt_line(t->vt, ay, &len);
        while (l && ax > 0 && ax - 1 < len && word_char(l[ax - 1].cp))
            ax--;
        l = vt_line(t->vt, by, &len);
        while (l && bx + 1 < len && word_char(l[bx + 1].cp) && word_char(l[bx].cp))
            bx++;
    } else if (t->sel_mode == 2) {
        ax = 0;
        bx = t->vt->cols - 1;
    }
    t->sel_valid = 1;
    t->sel_ay = ay; t->sel_ax = ax;
    t->sel_by = by; t->sel_bx = bx;
    widget_invalidate(t->canvas);
}

static char *selection_text(struct tab *t, int *out_len)
{
    if (!t->sel_valid)
        return NULL;
    size_t cap = 256, n = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    for (int line = t->sel_ay; line <= t->sel_by; line++) {
        int len;
        const struct vcell *l = vt_line(t->vt, line, &len);
        int from = line == t->sel_ay ? t->sel_ax : 0;
        int to = line == t->sel_by ? t->sel_bx : t->vt->cols - 1;
        if (to > len - 1) to = len - 1;
        while (to >= from && l[to].cp == ' ')
            to--;
        for (int c = from; c <= to; c++) {
            if (l[c].cp == VC_WIDE_TAIL)
                continue;
            char s[8];
            int k = gui_utf8_encode(l[c].cp ? l[c].cp : ' ', s);
            if (l[c].mark)
                k += gui_utf8_encode(l[c].mark, s + k);
            if (n + (size_t)k + 2 > cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) {
                    free(buf);
                    return NULL;
                }
                buf = nb;
            }
            memcpy(buf + n, s, (size_t)k);
            n += (size_t)k;
        }
        if (line != t->sel_by)
            buf[n++] = '\n';
    }
    buf[n] = '\0';
    *out_len = (int)n;
    return buf;
}

void write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t k = write(fd, s, n);
        if (k <= 0)
            return;
        s += k;
        n -= (size_t)k;
    }
}

void copy_selection(struct tab *t)
{
    int n;
    char *text = selection_text(t, &n);
    if (text) {
        gui_clipboard_set(text, n);
        free(text);
    }
}

void paste_clipboard(struct tab *t)
{
    char *buf = malloc(GUI_CLIP_MAX);
    if (!buf)
        return;
    int n = gui_clipboard_get(buf, GUI_CLIP_MAX);
    if (n > 0) {
        set_view(t, 0);
        if (t->vt->bracketed_paste)
            write_all(t->master, "\033[200~", 6);
        write_all(t->master, buf, (size_t)n);
        if (t->vt->bracketed_paste)
            write_all(t->master, "\033[201~", 6);
    }
    free(buf);
}

