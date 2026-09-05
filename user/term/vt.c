/* Terminal emulation: cells, scrollback, the alternate screen and the
 * escape sequence parser. See vt.h for the model and
 * docs/design/terminal.md for the sequences. */
#include "vt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_CHARSET, ST_DCS, ST_DCS_ESC };

/* The xterm palette as used by Visual Studio Code's dark theme. */
static const uint32_t palette[16] = {
    0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
    0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
};

uint32_t vt_palette(int index) { return palette[index & 15]; }

static uint32_t color256(int n)
{
    static const int level[6] = { 0, 95, 135, 175, 215, 255 };
    if (n < 0) n = 0;
    if (n < 16)
        return palette[n];
    if (n < 232) {
        n -= 16;
        return (uint32_t)level[n / 36] << 16 | (uint32_t)level[n / 6 % 6] << 8 | (uint32_t)level[n % 6];
    }
    if (n > 255) n = 255;
    uint32_t v = (uint32_t)(8 + (n - 232) * 10);
    return v << 16 | v << 8 | v;
}

static struct vcell *cell(struct vt *v, int r, int c) { return v->cells + r * v->cols + c; }

static void blank(struct vt *v, struct vcell *c, int n)
{
    for (int i = 0; i < n; i++) {
        c[i].cp = ' ';
        c[i].fg = v->fg;
        c[i].bg = v->bg;
        c[i].attr = 0;
    }
}

static void blank_all(struct vt *v, struct vcell *cells)
{
    uint32_t fg = v->fg, bg = v->bg;
    v->fg = VC_DEFAULT;
    v->bg = VC_DEFAULT;
    blank(v, cells, v->cols * v->rows);
    v->fg = fg;
    v->bg = bg;
}

/* ---- scrollback ---- */

static int is_blank(const struct vcell *c)
{
    return c->cp == ' ' && c->bg == VC_DEFAULT && !(c->attr & (VA_UNDERLINE | VA_STRIKE | VA_REVERSE));
}

static void sb_push(struct vt *v, const struct vcell *line)
{
    if (v->sb_cap <= 0)
        return;
    int n = v->cols;
    while (n > 0 && is_blank(&line[n - 1]))
        n--;
    struct vline *l;
    if (v->sb_count == v->sb_cap) {
        l = &v->sb[v->sb_head];
        v->sb_head = (v->sb_head + 1) % v->sb_cap;
    } else {
        l = &v->sb[(v->sb_head + v->sb_count) % v->sb_cap];
        v->sb_count++;
    }
    free(l->cells);
    l->cells = n ? malloc((size_t)n * sizeof *l->cells) : NULL;
    if (l->cells)
        memcpy(l->cells, line, (size_t)n * sizeof *l->cells);
    l->len = l->cells ? n : 0;
}

/* Take the newest scrollback line back onto the screen (resize). */
static int sb_pop(struct vt *v, struct vcell *line)
{
    if (v->sb_count == 0)
        return 0;
    v->sb_count--;
    struct vline *l = &v->sb[(v->sb_head + v->sb_count) % v->sb_cap];
    blank(v, line, v->cols);
    int n = l->len < v->cols ? l->len : v->cols;
    if (n)
        memcpy(line, l->cells, (size_t)n * sizeof *line);
    free(l->cells);
    l->cells = NULL;
    l->len = 0;
    return 1;
}

void vt_clear_scrollback(struct vt *v)
{
    for (int i = 0; i < v->sb_cap; i++) {
        free(v->sb[i].cells);
        v->sb[i].cells = NULL;
        v->sb[i].len = 0;
    }
    v->sb_count = v->sb_head = 0;
    v->dirty = 1;
}

int vt_total_lines(const struct vt *v) { return v->sb_count + v->rows; }

const struct vcell *vt_line(const struct vt *v, int index, int *len)
{
    if (index < 0 || index >= v->sb_count + v->rows) {
        *len = 0;
        return NULL;
    }
    if (index < v->sb_count) {
        const struct vline *l = &v->sb[(v->sb_head + index) % v->sb_cap];
        *len = l->len;
        return l->cells;
    }
    *len = v->cols;
    return v->cells + (index - v->sb_count) * v->cols;
}

/* ---- screen movement ---- */

static void scroll_up(struct vt *v, int top, int bot, int n)
{
    int height = bot - top + 1;
    if (n > height) n = height;
    if (n <= 0)
        return;
    if (top == 0 && v->cells == v->main)
        for (int i = 0; i < n; i++)
            sb_push(v, cell(v, i, 0));
    memmove(cell(v, top, 0), cell(v, top + n, 0), (size_t)(height - n) * v->cols * sizeof(struct vcell));
    blank(v, cell(v, bot - n + 1, 0), n * v->cols);
    v->dirty = 1;
}

static void scroll_down(struct vt *v, int top, int bot, int n)
{
    int height = bot - top + 1;
    if (n > height) n = height;
    if (n <= 0)
        return;
    memmove(cell(v, top + n, 0), cell(v, top, 0), (size_t)(height - n) * v->cols * sizeof(struct vcell));
    blank(v, cell(v, top, 0), n * v->cols);
    v->dirty = 1;
}

static void index_down(struct vt *v)
{
    if (v->cy == v->bot)
        scroll_up(v, v->top, v->bot, 1);
    else if (v->cy < v->rows - 1)
        v->cy++;
}

static void index_up(struct vt *v)
{
    if (v->cy == v->top)
        scroll_down(v, v->top, v->bot, 1);
    else if (v->cy > 0)
        v->cy--;
}

static void move_to(struct vt *v, int row, int col)
{
    int min_row = v->origin ? v->top : 0, max_row = v->origin ? v->bot : v->rows - 1;
    if (row < min_row) row = min_row;
    if (row > max_row) row = max_row;
    if (col < 0) col = 0;
    if (col > v->cols - 1) col = v->cols - 1;
    v->cy = row;
    v->cx = col;
    v->wrap_pending = 0;
}

static uint32_t current_fg(const struct vt *v)
{
    if (v->fg_index >= 0 && v->fg_index < 8 && (v->attr & VA_BOLD))
        return palette[v->fg_index + 8];
    return v->fg;
}

static void put_char(struct vt *v, uint32_t cp)
{
    if (v->wrap_pending) {
        v->wrap_pending = 0;
        if (v->autowrap) {
            v->cx = 0;
            index_down(v);
        }
    }
    if (v->insert && v->cx < v->cols - 1)
        memmove(cell(v, v->cy, v->cx + 1), cell(v, v->cy, v->cx), (size_t)(v->cols - 1 - v->cx) * sizeof(struct vcell));
    struct vcell *c = cell(v, v->cy, v->cx);
    c->cp = cp;
    c->fg = current_fg(v);
    c->bg = v->bg;
    c->attr = v->attr;
    if (v->cx < v->cols - 1)
        v->cx++;
    else
        v->wrap_pending = 1;
    v->dirty = 1;
}

static void erase_line(struct vt *v, int mode)
{
    if (mode == 0)
        blank(v, cell(v, v->cy, v->cx), v->cols - v->cx);
    else if (mode == 1)
        blank(v, cell(v, v->cy, 0), v->cx + 1);
    else
        blank(v, cell(v, v->cy, 0), v->cols);
    v->dirty = 1;
}

static void erase_display(struct vt *v, int mode)
{
    if (mode == 0) {
        erase_line(v, 0);
        if (v->cy < v->rows - 1)
            blank(v, cell(v, v->cy + 1, 0), (v->rows - 1 - v->cy) * v->cols);
    } else if (mode == 1) {
        blank(v, cell(v, 0, 0), v->cy * v->cols);
        erase_line(v, 1);
    } else if (mode == 2) {
        blank(v, cell(v, 0, 0), v->rows * v->cols);
    } else if (mode == 3) {
        vt_clear_scrollback(v);
    }
    v->dirty = 1;
}

static void save_cursor(struct vt *v)
{
    v->saved_cx = v->cx;
    v->saved_cy = v->cy;
    v->saved_fg = v->fg;
    v->saved_bg = v->bg;
    v->saved_fg_index = v->fg_index;
    v->saved_bg_index = v->bg_index;
    v->saved_attr = v->attr;
    v->saved_valid = 1;
}

static void restore_cursor(struct vt *v)
{
    if (!v->saved_valid)
        return;
    v->fg = v->saved_fg;
    v->bg = v->saved_bg;
    v->fg_index = v->saved_fg_index;
    v->bg_index = v->saved_bg_index;
    v->attr = v->saved_attr;
    move_to(v, v->saved_cy, v->saved_cx);
}

static void use_screen(struct vt *v, int alt)
{
    struct vcell *want = alt ? v->alt : v->main;
    if (v->cells == want)
        return;
    v->cells = want;
    v->top = 0;
    v->bot = v->rows - 1;
    v->wrap_pending = 0;
    v->dirty = 1;
}

static void reset(struct vt *v)
{
    v->fg = v->bg = VC_DEFAULT;
    v->fg_index = v->bg_index = -1;
    v->attr = 0;
    use_screen(v, 0);
    blank_all(v, v->main);
    blank_all(v, v->alt);
    v->cx = v->cy = 0;
    v->wrap_pending = 0;
    v->top = 0;
    v->bot = v->rows - 1;
    v->cursor_visible = 1;
    v->autowrap = 1;
    v->newline = 1;
    v->origin = v->insert = v->app_cursor = v->bracketed_paste = 0;
    v->saved_valid = 0;
    v->state = ST_GROUND;
    v->dirty = 1;
}

/* ---- sequences ---- */

static int param(const struct vt *v, int i, int fallback)
{
    return i < v->nparams && v->params[i] > 0 ? v->params[i] : fallback;
}

static void sgr(struct vt *v)
{
    if (v->nparams == 0) {
        v->params[0] = 0;
        v->nparams = 1;
    }
    for (int i = 0; i < v->nparams; i++) {
        int p = v->params[i];
        if (p == 0) {
            v->fg = v->bg = VC_DEFAULT;
            v->fg_index = v->bg_index = -1;
            v->attr = 0;
        } else if (p == 1) v->attr |= VA_BOLD;
        else if (p == 2) v->attr |= VA_FAINT;
        else if (p == 3) v->attr |= VA_ITALIC;
        else if (p == 4) v->attr |= VA_UNDERLINE;
        else if (p == 7) v->attr |= VA_REVERSE;
        else if (p == 9) v->attr |= VA_STRIKE;
        else if (p == 22) v->attr &= (uint8_t)~(VA_BOLD | VA_FAINT);
        else if (p == 23) v->attr &= (uint8_t)~VA_ITALIC;
        else if (p == 24) v->attr &= (uint8_t)~VA_UNDERLINE;
        else if (p == 27) v->attr &= (uint8_t)~VA_REVERSE;
        else if (p == 29) v->attr &= (uint8_t)~VA_STRIKE;
        else if (p >= 30 && p <= 37) { v->fg_index = p - 30; v->fg = palette[p - 30]; }
        else if (p >= 40 && p <= 47) { v->bg_index = p - 40; v->bg = palette[p - 40]; }
        else if (p >= 90 && p <= 97) { v->fg_index = p - 82; v->fg = palette[p - 82]; }
        else if (p >= 100 && p <= 107) { v->bg_index = p - 92; v->bg = palette[p - 92]; }
        else if (p == 39) { v->fg = VC_DEFAULT; v->fg_index = -1; }
        else if (p == 49) { v->bg = VC_DEFAULT; v->bg_index = -1; }
        else if (p == 38 || p == 48) {
            uint32_t c = 0;
            int ok = 0;
            if (i + 1 < v->nparams && v->params[i + 1] == 5 && i + 2 < v->nparams) {
                c = color256(v->params[i + 2]);
                i += 2;
                ok = 1;
            } else if (i + 1 < v->nparams && v->params[i + 1] == 2 && i + 4 < v->nparams) {
                c = (uint32_t)(v->params[i + 2] & 255) << 16 | (uint32_t)(v->params[i + 3] & 255) << 8 |
                    (uint32_t)(v->params[i + 4] & 255);
                i += 4;
                ok = 1;
            }
            if (ok && p == 38) { v->fg = c; v->fg_index = -1; }
            if (ok && p == 48) { v->bg = c; v->bg_index = -1; }
        }
    }
}

static void set_mode(struct vt *v, int on)
{
    for (int i = 0; i < v->nparams; i++) {
        int p = v->params[i];
        if (!v->priv) {
            if (p == 4)
                v->insert = on;
            else if (p == 20)
                v->newline = on;
            continue;
        }
        switch (p) {
        case 1: v->app_cursor = on; break;
        case 6: v->origin = on; move_to(v, on ? v->top : 0, 0); break;
        case 7: v->autowrap = on; break;
        case 25: v->cursor_visible = on; v->dirty = 1; break;
        case 47: case 1047:
            use_screen(v, on);
            if (on) blank_all(v, v->alt);
            break;
        case 1049:
            if (on) {
                save_cursor(v);
                use_screen(v, 1);
                blank_all(v, v->alt);
                move_to(v, 0, 0);
            } else {
                use_screen(v, 0);
                restore_cursor(v);
            }
            break;
        case 2004: v->bracketed_paste = on; break;
        }
    }
}

static void reply(struct vt *v, const char *s)
{
    int n = (int)strlen(s);
    if (v->reply_len + n < (int)sizeof v->reply) {
        memcpy(v->reply + v->reply_len, s, (size_t)n);
        v->reply_len += n;
    }
}

static void csi(struct vt *v, char cmd)
{
    int n = param(v, 0, 1);
    int in_region = v->cy >= v->top && v->cy <= v->bot;
    if (v->intermediate)                /* CSI ? q, CSI ! p and similar: not implemented */
        return;
    switch (cmd) {
    case '@':
        if (v->cx + n > v->cols) n = v->cols - v->cx;
        memmove(cell(v, v->cy, v->cx + n), cell(v, v->cy, v->cx), (size_t)(v->cols - v->cx - n) * sizeof(struct vcell));
        blank(v, cell(v, v->cy, v->cx), n);
        v->wrap_pending = 0;
        break;
    case 'A': {                         /* stops at the region edge when inside it */
        int limit = in_region ? v->top : 0;
        move_to(v, v->cy - n < limit ? limit : v->cy - n, v->cx);
        break;
    }
    case 'B': {
        int limit = in_region ? v->bot : v->rows - 1;
        move_to(v, v->cy + n > limit ? limit : v->cy + n, v->cx);
        break;
    }
    case 'C': move_to(v, v->cy, v->cx + n); break;
    case 'D': move_to(v, v->cy, v->cx - n); break;
    case 'E': move_to(v, v->cy + n, 0); break;
    case 'F': move_to(v, v->cy - n, 0); break;
    case 'G': case '`': move_to(v, v->cy, n - 1); break;
    case 'H': case 'f': move_to(v, (v->origin ? v->top : 0) + n - 1, param(v, 1, 1) - 1); break;
    case 'd': move_to(v, (v->origin ? v->top : 0) + n - 1, v->cx); break;
    case 'J': erase_display(v, param(v, 0, 0)); v->wrap_pending = 0; break;
    case 'K': erase_line(v, param(v, 0, 0)); v->wrap_pending = 0; break;
    case 'L': if (in_region) scroll_down(v, v->cy, v->bot, n); v->wrap_pending = 0; break;
    case 'M': if (in_region) scroll_up(v, v->cy, v->bot, n); v->wrap_pending = 0; break;
    case 'P':
        if (v->cx + n > v->cols) n = v->cols - v->cx;
        memmove(cell(v, v->cy, v->cx), cell(v, v->cy, v->cx + n), (size_t)(v->cols - v->cx - n) * sizeof(struct vcell));
        blank(v, cell(v, v->cy, v->cols - n), n);
        v->wrap_pending = 0;
        break;
    case 'S': scroll_up(v, v->top, v->bot, n); break;
    case 'T': scroll_down(v, v->top, v->bot, n); break;
    case 'X':
        if (v->cx + n > v->cols) n = v->cols - v->cx;
        blank(v, cell(v, v->cy, v->cx), n);
        break;
    case 'Z': move_to(v, v->cy, v->cx > 0 ? ((v->cx - 1) & ~7) : 0); break;
    case 'h': set_mode(v, 1); break;
    case 'l': set_mode(v, 0); break;
    case 'm': if (!v->priv) sgr(v); break;
    case 'n':
        if (param(v, 0, 0) == 5)
            reply(v, "\033[0n");
        else if (param(v, 0, 0) == 6) {
            char buf[32];
            snprintf(buf, sizeof buf, "\033[%d;%dR", v->cy + 1 - (v->origin ? v->top : 0), v->cx + 1);
            reply(v, buf);
        }
        break;
    case 'c': if (!v->priv) reply(v, "\033[?62;22c"); break;
    case 'r': {
        int top = param(v, 0, 1) - 1, bot = param(v, 1, v->rows) - 1;
        if (top < 0) top = 0;
        if (bot > v->rows - 1) bot = v->rows - 1;
        if (top < bot) {
            v->top = top;
            v->bot = bot;
            move_to(v, v->origin ? top : 0, 0);
        }
        break;
    }
    case 's': if (!v->priv) save_cursor(v); break;
    case 'u': if (!v->priv) restore_cursor(v); break;
    }
    v->dirty = 1;
}

static void esc(struct vt *v, char c)
{
    switch (c) {
    case '7': save_cursor(v); break;
    case '8': restore_cursor(v); break;
    case 'D': index_down(v); break;
    case 'E': v->cx = 0; index_down(v); break;
    case 'M': index_up(v); break;
    case 'c': reset(v); break;
    }
    v->wrap_pending = 0;
    v->dirty = 1;
}

static void osc_done(struct vt *v)
{
    v->osc[v->osc_len] = '\0';
    char *p = v->osc;
    int n = 0;
    while (*p >= '0' && *p <= '9')
        n = n * 10 + (*p++ - '0');
    if (*p == ';' && (n == 0 || n == 2)) {
        strlcpy(v->title, p + 1, sizeof v->title);
        v->title_changed = 1;
    }
}

static void control(struct vt *v, unsigned char c)
{
    switch (c) {
    case 7: v->bell = 1; break;
    case 8: if (v->cx > 0) v->cx--; v->wrap_pending = 0; break;
    case 9: {
        int nx = (v->cx + 8) & ~7;
        v->cx = nx < v->cols ? nx : v->cols - 1;
        v->wrap_pending = 0;
        break;
    }
    case 10: case 11: case 12:
        index_down(v);
        if (v->newline)
            v->cx = 0;
        v->wrap_pending = 0;
        break;
    case 13: v->cx = 0; v->wrap_pending = 0; break;
    }
    v->dirty = 1;
}

static void feed_byte(struct vt *v, unsigned char c)
{
    /* Control characters act inside escape sequences too, except in
     * strings (OSC, DCS) which they terminate or are part of. */
    if (c < 0x20 && v->state != ST_OSC && v->state != ST_DCS) {
        if (c == 27) {
            v->state = ST_ESC;
        } else if (c == 0x18 || c == 0x1a) {
            v->state = ST_GROUND;
        } else {
            control(v, c);
        }
        return;
    }
    switch (v->state) {
    case ST_ESC:
        v->nparams = 0;
        v->priv = 0;
        v->intermediate = 0;
        v->params[0] = 0;
        if (c == '[') v->state = ST_CSI;
        else if (c == ']') { v->state = ST_OSC; v->osc_len = 0; }
        else if (c == 'P' || c == '^' || c == '_') v->state = ST_DCS;
        else if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%') v->state = ST_CHARSET;
        else { esc(v, (char)c); v->state = ST_GROUND; }
        return;
    case ST_CHARSET:
        v->state = ST_GROUND;
        return;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (!v->nparams) v->nparams = 1;
            int *p = &v->params[v->nparams - 1];
            *p = *p > 100000 ? *p : *p * 10 + (c - '0');
        } else if (c == ';' || c == ':') {
            if (!v->nparams) v->nparams = 1;
            if (v->nparams < 16)
                v->params[v->nparams++] = 0;
        } else if (c == '?' || c == '>' || c == '=' || c == '<') {
            v->priv = c;
        } else if (c >= 0x20 && c <= 0x2f) {
            v->intermediate = c;
        } else if (c >= 0x40 && c <= 0x7e) {
            if (v->priv == '?' || !v->priv)
                csi(v, (char)c);
            v->state = ST_GROUND;
        } else {
            v->state = ST_GROUND;
        }
        return;
    case ST_OSC:
        if (c == 7) { osc_done(v); v->state = ST_GROUND; }
        else if (c == 27) v->state = ST_OSC_ESC;
        else if (v->osc_len < (int)sizeof v->osc - 1) v->osc[v->osc_len++] = (char)c;
        return;
    case ST_OSC_ESC:
        if (c == '\\') osc_done(v);
        v->state = ST_GROUND;
        return;
    case ST_DCS:
        if (c == 27) v->state = ST_DCS_ESC;
        else if (c == 7) v->state = ST_GROUND;
        return;
    case ST_DCS_ESC:
        v->state = c == '\\' ? ST_GROUND : ST_DCS;
        return;
    }
    /* ground: UTF-8 text */
    if (v->utf8_need) {
        if ((c & 0xc0) == 0x80) {
            v->utf8_cp = v->utf8_cp << 6 | (c & 0x3f);
            if (--v->utf8_need == 0)
                put_char(v, v->utf8_cp < 0x20 ? 0xfffd : v->utf8_cp);
            return;
        }
        v->utf8_need = 0;
        put_char(v, 0xfffd);
    }
    if (c < 0x80) {
        put_char(v, c);
    } else if ((c & 0xe0) == 0xc0) {
        v->utf8_cp = c & 0x1f;
        v->utf8_need = 1;
    } else if ((c & 0xf0) == 0xe0) {
        v->utf8_cp = c & 0x0f;
        v->utf8_need = 2;
    } else if ((c & 0xf8) == 0xf0) {
        v->utf8_cp = c & 0x07;
        v->utf8_need = 3;
    } else {
        put_char(v, 0xfffd);
    }
}

void vt_feed(struct vt *v, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        feed_byte(v, (unsigned char)data[i]);
}

int vt_take_reply(struct vt *v, char *buf, int size)
{
    int n = v->reply_len < size ? v->reply_len : size;
    memcpy(buf, v->reply, (size_t)n);
    v->reply_len = 0;
    return n;
}

/* ---- lifetime and size ---- */

struct vt *vt_create(int cols, int rows, int scrollback)
{
    struct vt *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->cols = cols;
    v->rows = rows;
    v->main = calloc((size_t)cols * rows, sizeof *v->main);
    v->alt = calloc((size_t)cols * rows, sizeof *v->alt);
    v->sb_cap = scrollback;
    v->sb = calloc((size_t)(scrollback > 0 ? scrollback : 1), sizeof *v->sb);
    if (!v->main || !v->alt || !v->sb) {
        vt_free(v);
        return NULL;
    }
    v->cells = v->main;
    reset(v);
    return v;
}

void vt_free(struct vt *v)
{
    if (!v)
        return;
    if (v->sb)
        vt_clear_scrollback(v);
    free(v->sb);
    free(v->main);
    free(v->alt);
    free(v);
}

static struct vcell *resize_screen(struct vt *v, struct vcell *old, int cols, int rows, int keep_rows)
{
    struct vcell *n = calloc((size_t)cols * rows, sizeof *n);
    if (!n)
        return NULL;
    uint32_t fg = v->fg, bg = v->bg;
    v->fg = v->bg = VC_DEFAULT;
    blank(v, n, cols * rows);
    v->fg = fg;
    v->bg = bg;
    int w = cols < v->cols ? cols : v->cols;
    for (int r = 0; r < keep_rows && r < rows; r++)
        memcpy(n + r * cols, old + r * v->cols, (size_t)w * sizeof *n);
    return n;
}

void vt_resize(struct vt *v, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == v->cols && rows == v->rows)
        return;
    int on_alt = v->cells == v->alt;
    /* Fewer rows: the lines above the cursor go to the scrollback
     * first, then lines below it are dropped. */
    int drop = v->rows - rows;
    struct vcell *saved = v->cells;
    v->cells = v->main;
    if (drop > 0) {
        int from_top = drop < v->cy ? drop : v->cy;
        for (int i = 0; i < from_top; i++)
            sb_push(v, cell(v, i, 0));
        memmove(cell(v, 0, 0), cell(v, from_top, 0), (size_t)(v->rows - from_top) * v->cols * sizeof(struct vcell));
        v->cy -= from_top;
    }
    struct vcell *nmain = resize_screen(v, v->main, cols, rows, v->rows);
    struct vcell *nalt = resize_screen(v, v->alt, cols, rows, v->rows);
    if (!nmain || !nalt) {
        free(nmain);
        free(nalt);
        v->cells = saved;
        return;
    }
    free(v->main);
    free(v->alt);
    v->main = nmain;
    v->alt = nalt;
    v->cols = cols;
    v->rows = rows;
    v->cells = v->main;
    /* More rows: pull lines back from the scrollback above the screen. */
    if (drop < 0 && !on_alt) {
        int grow = -drop;
        for (int i = 0; i < grow && v->sb_count > 0; i++) {
            memmove(cell(v, 1, 0), cell(v, 0, 0), (size_t)(v->rows - 1) * v->cols * sizeof(struct vcell));
            sb_pop(v, cell(v, 0, 0));
            v->cy++;
        }
    }
    v->cells = on_alt ? v->alt : v->main;
    v->top = 0;
    v->bot = rows - 1;
    if (v->cx > cols - 1) v->cx = cols - 1;
    if (v->cy > rows - 1) v->cy = rows - 1;
    if (v->saved_cx > cols - 1) v->saved_cx = cols - 1;
    if (v->saved_cy > rows - 1) v->saved_cy = rows - 1;
    v->wrap_pending = 0;
    v->dirty = 1;
}
