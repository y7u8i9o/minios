/* term: the terminal window. Each tab runs a program (the shell by
 * default) on a pseudo terminal and paints its emulator (vt.c) with the
 * DejaVu Sans Mono outline font. Keys, the mouse selection, the
 * clipboard, the scrollback bar and the context menu are here.
 *
 *   term [-d directory] [command [argument...]]
 *
 * Keys: Shift+PageUp/PageDown/Home/End scroll, Ctrl+Shift+C/V copy and
 * paste (also Shift+Insert and the middle button), Ctrl+Shift+T/W open
 * and close a tab, Ctrl+PageUp/PageDown switch tabs, Ctrl+plus/minus/0
 * change the font size. */
#include <minios/conf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/wait.h>
#include <gui/app.h>
#include <gui/i18n.h>
#include <gui/utf8.h>
#include "term.h"

static struct app *app;
static struct widget *win, *tabs, *menu;
static struct tab *tab_list[MAX_TABS];
static int ntabs, current;
static struct font *font;
static int font_px = DEFAULT_PX, sbw;
int cell_w, cell_h;
static int cols = 80, rows = 25;
static int focused = 1, blink_on = 1;
static struct timer *frame_timer;
static char start_dir[256];
static char **child_argv;

static struct tab *cur(void) { return ntabs ? tab_list[current] : NULL; }

/* ---- font and grid ---- */

static void load_font(int px)
{
    struct font *f = gfx_font_open_ttf(FONT_PATH, px);
    if (!f) {
        f = gfx_font_load("/usr/share/fonts/mono20.mfnt");
        if (!f) {
            font = NULL;
            cell_w = gfx_font_builtin()->advance['M'];
            cell_h = gfx_font_builtin()->height;
            return;
        }
    }
    if (font)
        gfx_font_free(font);
    font = f;
    font_px = px;
    cell_w = f->advance['M'];
    cell_h = f->height;
}

static const struct font *text_font(void) { return font ? font : gfx_font_builtin(); }

static void apply_grid(struct tab *t)
{
    vt_resize(t->vt, cols, rows);
    struct winsize ws = { (uint16_t)rows, (uint16_t)cols };
    ioctl(t->master, TIOCSWINSZ, &ws);
    if (t->view > t->vt->sb_count)
        t->view = t->vt->sb_count;
    widget_invalidate(t->canvas);
}

/* The grid follows the canvas of the current tab; every tab has the
 * same geometry because the pages share the window. */
static void grid_from_canvas(void)
{
    struct tab *t = cur();
    if (!t)
        return;
    window_paint(win);
    int w = t->canvas->w - 2 * PAD_X - sbw, h = t->canvas->h - 2 * PAD_Y;
    int nc = w / cell_w, nr = h / cell_h;
    if (nc < 1) nc = 1;
    if (nr < 1) nr = 1;
    if (nc == cols && nr == rows && t->vt->cols == nc && t->vt->rows == nr)
        return;
    cols = nc;
    rows = nr;
    for (int i = 0; i < ntabs; i++)
        apply_grid(tab_list[i]);
    printf("term: size %dx%d\n", cols, rows);
    fflush(stdout);
}

/* ---- painting ---- */

static uint32_t resolve(uint32_t c, uint32_t fallback) { return c & VC_DEFAULT ? fallback : c; }

static uint32_t mix(uint32_t a, uint32_t b)
{
    return ((a >> 1) & 0x7f7f7f) + ((b >> 1) & 0x7f7f7f);
}

/* cell_text encodes the character of a cell and its combining mark as
 * UTF-8 into s, which has room for 12 bytes, and returns the length. */
static int cell_text(const struct vcell *c, char *s)
{
    int n = gui_utf8_encode(c->cp, s);
    if (c->mark)
        n += gui_utf8_encode(c->mark, s + n);
    return n;
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct tab *t = arg;
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct vt *v = t->vt;
    painter_fill(p, 0, 0, w->w, w->h, DEFAULT_BG);
    struct rect clip = painter_clip_local(p);
    int total = vt_total_lines(v);
    int first = total - v->rows - t->view;
    char s[12];
    for (int r = 0; r < v->rows; r++) {
        int y = PAD_Y + r * cell_h;
        if (y + cell_h <= clip.y || y >= clip.y + clip.h)
            continue;
        int len;
        const struct vcell *line = vt_line(v, first + r, &len);
        for (int c = 0; c < len && c < v->cols; c++) {
            const struct vcell *cell = &line[c];
            uint32_t fg = resolve(cell->fg, DEFAULT_FG), bg = resolve(cell->bg, DEFAULT_BG);
            if (cell->attr & VA_REVERSE) {
                uint32_t x = fg;
                fg = bg;
                bg = x;
            }
            if (selected(t, first + r, c))
                bg = SELECTION_BG;
            if (cell->attr & VA_FAINT)
                fg = mix(fg, bg);
            int x = PAD_X + c * cell_w;
            /* Runs of one background are filled at once. */
            int run = 1;
            while (c + run < len && c + run < v->cols) {
                const struct vcell *n = &line[c + run];
                uint32_t nbg = n->attr & VA_REVERSE ? resolve(n->fg, DEFAULT_FG) : resolve(n->bg, DEFAULT_BG);
                if (selected(t, first + r, c + run))
                    nbg = SELECTION_BG;
                if (nbg != bg)
                    break;
                run++;
            }
            if (bg != DEFAULT_BG)
                painter_fill(p, x, y, run * cell_w, cell_h, bg);
            for (int k = 0; k < run; k++) {
                const struct vcell *cc = &line[c + k];
                uint32_t cfg = resolve(cc->fg, DEFAULT_FG);
                if (cc->attr & VA_REVERSE)
                    cfg = resolve(cc->bg, DEFAULT_BG);
                if (cc->attr & VA_FAINT)
                    cfg = mix(cfg, bg);
                int cx = x + k * cell_w;
                if (cc->cp != ' ' && cc->cp && cc->cp != VC_WIDE_TAIL) {
                    int n = cell_text(cc, s);
                    s[n] = '\0';
                    painter_text_font(p, text_font(), cx, y, s, cfg, 0xffffffffu);
                }
                if (cc->attr & VA_UNDERLINE)
                    painter_fill(p, cx, y + cell_h - 2, cell_w, 1, cfg);
                if (cc->attr & VA_STRIKE)
                    painter_fill(p, cx, y + cell_h / 2, cell_w, 1, cfg);
            }
            c += run - 1;
        }
    }
    if (t->view == 0 && v->cursor_visible && focused)
        widget_text_cursor(w, PAD_X + v->cx * cell_w, PAD_Y + v->cy * cell_h, 1, cell_h);
    if (t->view == 0 && t->preedit[0]) {
        /* The composition covers the cells from the cursor, underlined. */
        int x = PAD_X + v->cx * cell_w, y = PAD_Y + v->cy * cell_h;
        int pw = gfx_text_width_font(text_font(), t->preedit, -1);
        painter_fill(p, x, y, pw, cell_h, DEFAULT_BG);
        painter_text_font(p, text_font(), x, y, t->preedit, DEFAULT_FG, 0xffffffffu);
        painter_fill(p, x, y + cell_h - 1, pw, 1, DEFAULT_FG);
    } else if (t->view == 0 && v->cursor_visible) {
        int x = PAD_X + v->cx * cell_w, y = PAD_Y + v->cy * cell_h;
        const struct vcell *cc = v->cells + v->cy * v->cols + v->cx;
        if (focused && blink_on) {
            painter_fill(p, x, y, cell_w, cell_h, CURSOR);
            if (cc->cp != ' ' && cc->cp && cc->cp != VC_WIDE_TAIL) {
                int n = cell_text(cc, s);
                s[n] = '\0';
                painter_text_font(p, text_font(), x, y, s, DEFAULT_BG, 0xffffffffu);
            }
        } else if (!focused) {
            painter_frame(p, x, y, cell_w, cell_h, CURSOR);
        }
    }
    scrollbar_paint_track(p, w->w - sbw, 0, sbw, w->h, v->sb_count - t->view, total, v->rows, 1);
    return 1;
}

/* Paint at most once per frame while output streams in. */
static void on_frame(void *arg)
{
    frame_timer = NULL;
    for (int i = 0; i < ntabs; i++) {
        struct tab *t = tab_list[i];
        if (t->vt->dirty) {
            t->vt->dirty = 0;
            widget_invalidate(t->canvas);
        }
        if (t->vt->title_changed) {
            t->vt->title_changed = 0;
            strlcpy(t->title, t->vt->title, sizeof t->title);
            widget_set_text(t->page, t->title);
            widget_invalidate(tabs);
            if (i == current) {
                char title[96];
                snprintf(title, sizeof title, _("%s - Terminal"), t->title);
                gui_set_title(window_state_of(win)->win, title);
            }
        }
    }
}

static void schedule_frame(void)
{
    if (!frame_timer)
        frame_timer = app_timer_add(app, 16, 0, on_frame, NULL);
}

static void on_blink(void *arg)
{
    blink_on = !blink_on;
    struct tab *t = cur();
    if (t && focused && t->view == 0 && t->vt->cursor_visible)
        widget_invalidate(t->canvas);
}

/* ---- scrolling ---- */

void set_view(struct tab *t, int v)
{
    if (v < 0) v = 0;
    if (v > t->vt->sb_count) v = t->vt->sb_count;
    if (v == t->view)
        return;
    t->view = v;
    widget_invalidate(t->canvas);
    printf("term: view %d\n", t->view);
    fflush(stdout);
}

/* ---- tabs ---- */

static void update_title(void)
{
    struct tab *t = cur();
    char title[96];
    if (t && t->vt->title[0])
        snprintf(title, sizeof title, _("%s - Terminal"), t->title);
    else
        strlcpy(title, _("Terminal"), sizeof title);
    gui_set_title(window_state_of(win)->win, title);
}

static void close_tab(struct tab *t)
{
    int i = 0;
    while (i < ntabs && tab_list[i] != t)
        i++;
    if (i == ntabs)
        return;
    app_unwatch_fd(app, t->watch);
    close(t->master);
    if (t->pid > 0) {
        kill(-t->pid, SIGHUP);
        kill(-t->pid, SIGKILL);
        waitpid(t->pid, NULL, 0);
    }
    vt_free(t->vt);
    widget_destroy(t->page);
    free(t);
    memmove(tab_list + i, tab_list + i + 1, (size_t)(ntabs - i - 1) * sizeof *tab_list);
    ntabs--;
    if (ntabs == 0) {
        app_quit(app, 0);
        return;
    }
    if (current >= ntabs)
        current = ntabs - 1;
    tabs->value = -1;
    tabs_select(tabs, current);
    widget_focus(cur()->canvas);
    update_title();
}

static void reap_tab(struct tab *t)
{
    int status;
    if (t->pid > 0 && waitpid(t->pid, &status, WNOHANG) == t->pid) {
        printf("term: shell exited, status 0x%x\n", status);
        fflush(stdout);
        t->pid = 0;
        close_tab(t);
    }
}

static void on_master(int fd, int revents, void *arg)
{
    struct tab *t = arg;
    char buf[4096];
    int got = 0;
    /* The pseudo terminal ignores O_NONBLOCK: read only what poll
     * reports, a few buffers per wake-up. */
    for (int i = 0; i < 16; i++) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (i > 0 && poll(&pfd, 1, 0) <= 0)
            break;
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        got = 1;
        vt_feed(t->vt, buf, (size_t)n);
        char reply[64];
        int rn = vt_take_reply(t->vt, reply, sizeof reply);
        if (rn)
            write_all(fd, reply, (size_t)rn);
    }
    if (got) {
        if (t->view)
            set_view(t, 0);
        if (t->vt->bell)
            t->vt->bell = 0;
        schedule_frame();
    }
    reap_tab(t);
}

static void on_poll(void *arg)
{
    for (int i = 0; i < ntabs; i++)
        reap_tab(tab_list[i]);
}

static int on_key(struct widget *w, void *args, void *arg);
static int on_text(struct widget *w, void *args, void *arg);
static int on_preedit(struct widget *w, void *args, void *arg);
static int on_press(struct widget *w, void *args, void *arg);
static int on_motion(struct widget *w, void *args, void *arg);
static int on_release(struct widget *w, void *args, void *arg);
static int on_wheel(struct widget *w, void *args, void *arg);

static struct tab *open_tab(char *const argv[], const char *dir)
{
    if (ntabs == MAX_TABS)
        return NULL;
    struct tab *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->vt = vt_create(cols, rows, SCROLLBACK);
    char slave[32];
    int slave_fd = -1;
    if (!t->vt || openpty(&t->master, &slave_fd, slave, NULL, NULL) < 0) {
        perror("term: openpty");
        vt_free(t->vt);
        free(t);
        return NULL;
    }
    /* The child opens the slave by its path. */
    close(slave_fd);
    struct winsize ws = { (uint16_t)rows, (uint16_t)cols };
    ioctl(t->master, TIOCSWINSZ, &ws);
    const char *name = strrchr(argv[0], '/');
    strlcpy(t->title, name ? name + 1 : argv[0], sizeof t->title);
    t->page = tabs_add(tabs, t->title);
    widget_set_padding(t->page, 0);
    t->canvas = canvas_new(t->page);
    widget_set_stretch(t->canvas, 1, 1);
    widget_connect(t->canvas, "paint", on_paint, t);
    /* Text input: the compositor or the input method commits the text
     * of the printable keys, which goes to the pty. */
    t->canvas->accepts_text = 1;
    widget_connect(t->canvas, "key", on_key, t);
    widget_connect(t->canvas, "text", on_text, t);
    widget_connect(t->canvas, "preedit", on_preedit, t);
    widget_connect(t->canvas, "press", on_press, t);
    widget_connect(t->canvas, "motion", on_motion, t);
    widget_connect(t->canvas, "release", on_release, t);
    widget_connect(t->canvas, "wheel", on_wheel, t);
    tab_list[ntabs++] = t;
    t->pid = fork();
    if (t->pid == 0) {
        setpgid(0, 0);
        int s = open(slave, O_RDWR);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        close(s);
        for (int i = 0; i < ntabs; i++)
            close(tab_list[i]->master);
        tcsetpgrp(0, getpgrp());
        setenv("TERM", "xterm-256color", 1);
        /* The shell runs as the account of the terminal window's user. */
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            setenv("HOME", pw->pw_dir, 1);
            setenv("USER", pw->pw_name, 1);
            setenv("LOGNAME", pw->pw_name, 1);
            setenv("SHELL", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh", 1);
        }
        if (dir && dir[0])
            chdir(dir);
        execvp(argv[0], argv);
        _exit(127);
    }
    printf("term: shell pid %d on %s, %dx%d cells of %dx%d\n", t->pid, slave, cols, rows, cell_w, cell_h);
    fflush(stdout);
    t->watch = app_watch_fd(app, t->master, POLLIN, on_master, t);
    current = ntabs - 1;
    tabs_select(tabs, current);
    widget_focus(t->canvas);
    update_title();
    return t;
}

static int on_tab_changed(struct widget *w, void *args, void *arg)
{
    current = ((struct sig_select *)args)->index;
    if (cur())
        widget_focus(cur()->canvas);
    update_title();
    grid_from_canvas();
    return 1;
}

static void new_tab(void)
{
    open_tab(child_argv, start_dir);
}

/* ---- zoom ---- */

static void set_font_px(int px)
{
    if (px < 6 || px > 40)
        return;
    load_font(px);
    for (int i = 0; i < ntabs; i++)
        widget_invalidate(tab_list[i]->canvas);
    cols = rows = 0;
    grid_from_canvas();
    gui_set_min_size(window_state_of(win)->win, MIN_COLS * cell_w + 2 * PAD_X + sbw, MIN_ROWS * cell_h + 2 * PAD_Y);
}

/* ---- input ---- */

static void send(struct tab *t, const char *s) { write_all(t->master, s, strlen(s)); }

static int on_key(struct widget *w, void *args, void *arg)
{
    struct tab *t = arg;
    struct sig_key *k = args;
    int code = k->code, ch = k->ch, mods = k->mods;
    int shift = mods & WMOD_SHIFT, ctrl = mods & WMOD_CTRL, alt = mods & WMOD_ALT;
    blink_on = 1;
    if (ctrl && shift) {
        switch (code) {
        case KEY_C: copy_selection(t); return 1;
        case KEY_V: paste_clipboard(t); return 1;
        case KEY_T: new_tab(); return 1;
        case KEY_W: close_tab(t); return 1;
        }
    }
    if (ctrl && !alt) {
        if (code == KEY_EQUAL || code == KEY_KPPLUS) { set_font_px(font_px + 1); return 1; }
        if (code == KEY_MINUS || code == KEY_KPMINUS) { set_font_px(font_px - 1); return 1; }
        if (code == KEY_0 || code == KEY_KP0) { set_font_px(DEFAULT_PX); return 1; }
        if (code == KEY_PAGEUP || code == KEY_PAGEDOWN) {
            if (ntabs > 1)
                tabs_select(tabs, (current + (code == KEY_PAGEUP ? ntabs - 1 : 1)) % ntabs);
            return 1;
        }
    }
    if (shift && !ctrl) {
        switch (code) {
        case KEY_PAGEUP: set_view(t, t->view + rows / 2); return 1;
        case KEY_PAGEDOWN: set_view(t, t->view - rows / 2); return 1;
        case KEY_HOME: set_view(t, t->vt->sb_count); return 1;
        case KEY_END: set_view(t, 0); return 1;
        case KEY_INSERT: paste_clipboard(t); return 1;
        }
    }
    int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    char seq[16];
    const char *letter = NULL, *tilde = NULL;
    switch (code) {
    case KEY_UP: letter = "A"; break;
    case KEY_DOWN: letter = "B"; break;
    case KEY_RIGHT: letter = "C"; break;
    case KEY_LEFT: letter = "D"; break;
    case KEY_HOME: letter = "H"; break;
    case KEY_END: letter = "F"; break;
    case KEY_F1: letter = "P"; break;
    case KEY_F2: letter = "Q"; break;
    case KEY_F3: letter = "R"; break;
    case KEY_F4: letter = "S"; break;
    case KEY_INSERT: tilde = "2"; break;
    case KEY_DELETE: tilde = "3"; break;
    case KEY_PAGEUP: tilde = "5"; break;
    case KEY_PAGEDOWN: tilde = "6"; break;
    case KEY_F5: tilde = "15"; break;
    case KEY_F6: tilde = "17"; break;
    case KEY_F7: tilde = "18"; break;
    case KEY_F8: tilde = "19"; break;
    case KEY_F9: tilde = "20"; break;
    case KEY_F10: tilde = "21"; break;
    case KEY_F11: tilde = "23"; break;
    case KEY_F12: tilde = "24"; break;
    }
    set_view(t, 0);
    if (letter) {
        int fkey = code >= KEY_F1 && code <= KEY_F4;
        if (m > 1)
            snprintf(seq, sizeof seq, "\033[1;%d%s", m, letter);
        else if (fkey || (t->vt->app_cursor && !(code == KEY_HOME || code == KEY_END)))
            snprintf(seq, sizeof seq, "\033O%s", letter);
        else
            snprintf(seq, sizeof seq, "\033[%s", letter);
        send(t, seq);
        return 1;
    }
    if (tilde) {
        if (m > 1)
            snprintf(seq, sizeof seq, "\033[%s;%d~", tilde, m);
        else
            snprintf(seq, sizeof seq, "\033[%s~", tilde);
        send(t, seq);
        return 1;
    }
    if (!ch && code == KEY_ESC)
        ch = 27;
    if (ch) {
        char s[8];
        int n = 0;
        if (alt)
            s[n++] = 27;
        if (ch < 0x80)
            s[n++] = (char)ch;
        else
            n += gui_utf8_encode((uint32_t)ch, s + n);
        write_all(t->master, s, (size_t)n);
    }
    return 1;
}

static int on_text(struct widget *w, void *args, void *arg)
{
    struct tab *t = arg;
    const char *text = ((struct sig_text *)args)->text;
    t->preedit[0] = '\0';
    blink_on = 1;
    set_view(t, 0);
    write_all(t->master, text, strlen(text));
    widget_invalidate(w);
    return 1;
}

static int on_preedit(struct widget *w, void *args, void *arg)
{
    struct tab *t = arg;
    strlcpy(t->preedit, ((struct sig_text *)args)->text, sizeof t->preedit);
    widget_invalidate(w);
    return 1;
}

static int on_menu_copy(struct widget *w, void *args, void *arg) { if (cur()) copy_selection(cur()); return 1; }
static int on_menu_paste(struct widget *w, void *args, void *arg) { if (cur()) paste_clipboard(cur()); return 1; }
static int on_menu_new_tab(struct widget *w, void *args, void *arg) { new_tab(); return 1; }
static int on_menu_close_tab(struct widget *w, void *args, void *arg) { if (cur()) close_tab(cur()); return 1; }
static int on_menu_zoom_in(struct widget *w, void *args, void *arg) { set_font_px(font_px + 1); return 1; }
static int on_menu_zoom_out(struct widget *w, void *args, void *arg) { set_font_px(font_px - 1); return 1; }
static int on_menu_zoom_reset(struct widget *w, void *args, void *arg) { set_font_px(DEFAULT_PX); return 1; }
static int on_menu_clear(struct widget *w, void *args, void *arg)
{
    struct tab *t = cur();
    if (t) {
        vt_clear_scrollback(t->vt);
        clear_selection(t);
        set_view(t, 0);
        widget_invalidate(t->canvas);
    }
    return 1;
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct tab *t = arg;
    struct sig_click *c = args;
    widget_focus(w);
    if (c->button & 2) {
        int x, y;
        widget_abs(w, &x, &y);
        menu_popup(menu, x + c->x, y + c->y);
        return 1;
    }
    if (c->button & 4) {
        paste_clipboard(t);
        return 1;
    }
    if (!(c->button & 1))
        return 1;
    if (c->x >= w->w - sbw) {
        /* The bar: jump so that the clicked point is the centre of the page. */
        int total = vt_total_lines(t->vt);
        int line = (int)((long)c->y * total / (w->h > 0 ? w->h : 1)) - t->vt->rows / 2;
        set_view(t, t->vt->sb_count - line);
        t->selecting = 2;
        return 1;
    }
    long now = uptime_ms();
    t->clicks = now - t->click_ms < 400 ? t->clicks + 1 : 1;
    t->click_ms = now;
    int line, col;
    cell_at(t, c->x, c->y, &line, &col);
    t->anchor_y = line;
    t->anchor_x = col;
    t->sel_mode = t->clicks >= 3 ? 2 : t->clicks == 2 ? 1 : 0;
    t->selecting = 1;
    if (t->sel_mode)
        extend_selection(t, line, col);
    else
        clear_selection(t);
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct tab *t = arg;
    struct sig_click *c = args;
    if (!(c->button & 1) || !t->selecting)
        return 0;
    if (t->selecting == 2) {
        int total = vt_total_lines(t->vt);
        int line = (int)((long)c->y * total / (w->h > 0 ? w->h : 1)) - t->vt->rows / 2;
        set_view(t, t->vt->sb_count - line);
        return 1;
    }
    int line, col;
    cell_at(t, c->x, c->y, &line, &col);
    if (line != t->anchor_y || col != t->anchor_x || t->sel_mode)
        extend_selection(t, line, col);
    return 1;
}

static int on_release(struct widget *w, void *args, void *arg)
{
    struct tab *t = arg;
    t->selecting = 0;
    return 1;
}

static int on_wheel(struct widget *w, void *args, void *arg)
{
    struct tab *t = arg;
    set_view(t, t->view - 3 * ((struct sig_click *)args)->button);
    return 1;
}

static int on_resize(struct widget *w, void *args, void *arg)
{
    grid_from_canvas();
    return 0;
}

static int on_focus(struct widget *w, void *args, void *arg)
{
    focused = ((struct sig_change *)args)->value;
    blink_on = 1;
    if (cur())
        widget_invalidate(cur()->canvas);
    return 0;
}

static int on_close(struct widget *w, void *args, void *arg)
{
    app_quit(app, 0);
    return 1;
}

/* term_font_px from the configuration file, written by the settings program. */
static int conf_font_px(void)
{
    char path[256];
    FILE *f = fopen(conf_read_path(path, sizeof path), "r");
    int px = DEFAULT_PX;
    if (!f)
        return px;
    char line[128];
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "term_font_px=", 13) == 0) {
            int v = atoi(line + 13);
            if (v >= 6 && v <= 40)
                px = v;
        }
    fclose(f);
    return px;
}

int main(int argc, char **argv)
{
    static char *const shell_argv[] = { "sh", NULL };
    int i = 1;
    if (argc > 2 && strcmp(argv[1], "-d") == 0) {
        strlcpy(start_dir, argv[2], sizeof start_dir);
        i = 3;
    }
    child_argv = i < argc ? argv + i : (char **)shell_argv;
    app = app_create();
    if (!app) {
        fprintf(stderr, "term: no window server\n");
        return 1;
    }
    textdomain("term");
    sbw = theme_px(app_theme(app), TM_SCROLLBAR);
    load_font(conf_font_px());
    win = app_window(app, cols * cell_w + 2 * PAD_X + sbw, rows * cell_h + 2 * PAD_Y, _("Terminal"));
    if (!win)
        return 1;
    widget_set_padding(win, 0);
    gui_set_min_size(window_state_of(win)->win, MIN_COLS * cell_w + 2 * PAD_X + sbw, MIN_ROWS * cell_h + 2 * PAD_Y);
    tabs = tabs_new(win);
    tabs_set_autohide(tabs, 1);
    tabs->focusable = 0;
    widget_connect(tabs, "changed", on_tab_changed, NULL);
    widget_connect(win, "resize", on_resize, NULL);
    widget_connect(win, "focus", on_focus, NULL);
    widget_connect(win, "close", on_close, NULL);
    menu = popupmenu_new(win);
    widget_connect(menu_add(menu, _("Copy"), "copy"), "clicked", on_menu_copy, NULL);
    widget_connect(menu_add(menu, _("Paste"), "paste"), "clicked", on_menu_paste, NULL);
    menu_add_separator(menu);
    widget_connect(menu_add(menu, _("New tab"), "new"), "clicked", on_menu_new_tab, NULL);
    widget_connect(menu_add(menu, _("Close tab"), NULL), "clicked", on_menu_close_tab, NULL);
    menu_add_separator(menu);
    widget_connect(menu_add(menu, _("Larger text"), NULL), "clicked", on_menu_zoom_in, NULL);
    widget_connect(menu_add(menu, _("Smaller text"), NULL), "clicked", on_menu_zoom_out, NULL);
    widget_connect(menu_add(menu, _("Normal size"), NULL), "clicked", on_menu_zoom_reset, NULL);
    menu_add_separator(menu);
    widget_connect(menu_add(menu, _("Clear scrollback"), NULL), "clicked", on_menu_clear, NULL);
    if (!open_tab(child_argv, start_dir))
        return 1;
    grid_from_canvas();
    app_timer_add(app, 200, 1, on_poll, NULL);
    app_timer_add(app, 530, 1, on_blink, NULL);
    app_run(app);
    while (ntabs > 0) {
        struct tab *t = tab_list[ntabs - 1];
        app_unwatch_fd(app, t->watch);
        close(t->master);
        if (t->pid > 0) {
            kill(-t->pid, SIGHUP);
            kill(-t->pid, SIGKILL);
            waitpid(t->pid, NULL, 0);
        }
        vt_free(t->vt);
        free(t);
        ntabs--;
    }
    app_destroy(app);
    return 0;
}
