/* term: a terminal emulator window on the application framework. The
 * shell runs on a pseudo terminal watched by the application loop; a
 * canvas renders a VT100 subset with the mono20 font. The cell grid
 * follows the canvas size (TIOCSWINSZ tells the shell); lines scrolled
 * off the top go to a scrollback viewed with the wheel or
 * Shift+PageUp/PageDown. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/wait.h>
#include <gui/app.h>

#define FG 0x00e0e0e0
#define BG 0x00101010
#define SCROLLBACK 1000
#define MIN_COLS 20
#define MIN_ROWS 5

static int cols = 80, rows = 25;
static char *cells;
static int cx, cy;
static int esc_state, esc_args[4], esc_nargs;
static struct app *app;
static struct widget *win, *canvas;
static const struct font *font;
static int cell_w, cell_h;
static int master;
static pid_t shell;
static char *sb[SCROLLBACK];
static int sb_len[SCROLLBACK];
static int sb_count, sb_head;
static int view;

static char *cell(int r, int c) { return cells + r * cols + c; }

static void scrollback_push(const char *line, int n)
{
    int idx = (sb_head + sb_count) % SCROLLBACK;
    if (sb_count == SCROLLBACK) {
        free(sb[sb_head]);
        idx = sb_head;
        sb_head = (sb_head + 1) % SCROLLBACK;
    } else {
        sb_count++;
    }
    while (n > 0 && line[n - 1] == ' ')
        n--;
    sb[idx] = malloc((size_t)n + 1);
    memcpy(sb[idx], line, (size_t)n);
    sb[idx][n] = '\0';
    sb_len[idx] = n;
}

static void scroll(void)
{
    scrollback_push(cell(0, 0), cols);
    memmove(cell(0, 0), cell(1, 0), (size_t)(rows - 1) * cols);
    memset(cell(rows - 1, 0), ' ', (size_t)cols);
}

static void csi(char cmd)
{
    int a0 = esc_nargs > 0 ? esc_args[0] : 0, a1 = esc_nargs > 1 ? esc_args[1] : 0;
    switch (cmd) {
    case 'H': case 'f':
        cy = a0 ? (a0 > rows ? rows : a0) - 1 : 0;
        cx = a1 ? (a1 > cols ? cols : a1) - 1 : 0;
        break;
    case 'J':
        if (a0 == 2) {
            memset(cells, ' ', (size_t)rows * cols);
            cx = cy = 0;
        } else {
            memset(cell(cy, cx), ' ', (size_t)(cols - cx));
            for (int r = cy + 1; r < rows; r++)
                memset(cell(r, 0), ' ', (size_t)cols);
        }
        break;
    case 'K': memset(cell(cy, cx), ' ', (size_t)(cols - cx)); break;
    case 'A': cy -= (a0 ? a0 : 1); if (cy < 0) cy = 0; break;
    case 'B': cy += (a0 ? a0 : 1); if (cy >= rows) cy = rows - 1; break;
    case 'C': cx += (a0 ? a0 : 1); if (cx >= cols) cx = cols - 1; break;
    case 'D': cx -= (a0 ? a0 : 1); if (cx < 0) cx = 0; break;
    }
}

static void put(char c)
{
    if (esc_state == 1) {
        esc_state = c == '[' ? 2 : 0;
        esc_nargs = 0;
        esc_args[0] = 0;
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (!esc_nargs) esc_nargs = 1;
            esc_args[esc_nargs - 1] = esc_args[esc_nargs - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (esc_nargs < 4) {
                if (!esc_nargs) esc_nargs = 1;
                esc_args[esc_nargs++] = 0;
            }
        } else if (c != '?') {
            csi(c);
            esc_state = 0;
        }
        return;
    }
    switch (c) {
    case 27: esc_state = 1; return;
    case '\n': cx = 0; cy++; break;
    case '\r': cx = 0; break;
    case '\b': if (cx > 0) cx--; break;
    case '\t': cx = (cx + 8) & ~7; break;
    default:
        if ((unsigned char)c < 32)
            break;
        *cell(cy, cx++) = c;
        break;
    }
    if (cx >= cols) {
        cx = 0;
        cy++;
    }
    if (cy >= rows) {
        scroll();
        cy = rows - 1;
    }
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, BG);
    if (!cells)
        return 1;
    char line[512];
    for (int r = 0; r < rows; r++) {
        int src = r - view;
        const char *text;
        int n;
        if (src < 0) {
            int idx = ((sb_head + sb_count + src) % SCROLLBACK + SCROLLBACK) % SCROLLBACK;
            text = sb[idx];
            n = sb_len[idx];
        } else {
            text = cell(src, 0);
            n = cols;
        }
        if (n > (int)sizeof line - 1)
            n = (int)sizeof line - 1;
        memcpy(line, text, (size_t)n);
        line[n] = '\0';
        painter_text_font(p, font, 0, r * cell_h, line, FG, BG);
    }
    if (!view) {
        char s[2] = { *cell(cy, cx), 0 };
        painter_text_font(p, font, cx * cell_w, cy * cell_h, s, BG, FG);
    }
    return 1;
}

static void set_view(int v)
{
    if (v < 0) v = 0;
    if (v > sb_count) v = sb_count;
    if (v == view)
        return;
    view = v;
    widget_invalidate(canvas);
    printf("term: view %d\n", view);
    fflush(stdout);
}

static void resize_cells(int width, int height)
{
    int ncols = width / cell_w, nrows = height / cell_h;
    if (ncols < 1) ncols = 1;
    if (nrows < 1) nrows = 1;
    if (ncols == cols && nrows == rows && cells)
        return;
    char *ncells = malloc((size_t)ncols * nrows);
    memset(ncells, ' ', (size_t)ncols * nrows);
    for (int r = 0; cells && r < rows && r < nrows; r++)
        memcpy(ncells + r * ncols, cell(r, 0), (size_t)(cols < ncols ? cols : ncols));
    free(cells);
    cells = ncells;
    cols = ncols;
    rows = nrows;
    if (cx >= cols) cx = cols - 1;
    if (cy >= rows) cy = rows - 1;
    struct winsize ws = { (uint16_t)rows, (uint16_t)cols };
    ioctl(master, TIOCSWINSZ, &ws);
    widget_invalidate(canvas);
    printf("term: size %dx%d\n", cols, rows);
    fflush(stdout);
}

static int on_resize(struct widget *w, void *args, void *arg)
{
    /* The canvas gets its new size at the next layout; measure then. */
    window_paint(win);
    resize_cells(canvas->w, canvas->h);
    return 0;
}

static int send_key(int code, int ch, int mods)
{
    if ((mods & WMOD_SHIFT) && (code == 0xc9 || code == 0xd1)) {
        set_view(view + (code == 0xc9 ? rows / 2 : -rows / 2));
        return 1;
    }
    const char *seq = NULL;
    switch (code) {
    case 0xc8: seq = "\033[A"; break;
    case 0xd0: seq = "\033[B"; break;
    case 0xcd: seq = "\033[C"; break;
    case 0xcb: seq = "\033[D"; break;
    case 0xc7: seq = "\033[H"; break;
    case 0xcf: seq = "\033[F"; break;
    case 0xd3: seq = "\033[3~"; break;
    case 0xc9: seq = "\033[5~"; break;
    case 0xd1: seq = "\033[6~"; break;
    }
    set_view(0);
    if (seq) {
        write(master, seq, strlen(seq));
        return 1;
    }
    if (ch) {
        char c = (char)ch;
        write(master, &c, 1);
    }
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    return send_key(k->code, k->ch, k->mods);
}

static int on_wheel(struct widget *w, void *args, void *arg)
{
    set_view(view - 3 * ((struct sig_click *)args)->button);
    return 1;
}

static void on_master(int fd, int revents, void *arg)
{
    char buf[512];
    ssize_t n = read(master, buf, sizeof buf);
    if (n > 0) {
        set_view(0);
        for (ssize_t i = 0; i < n; i++)
            put(buf[i]);
        widget_invalidate(canvas);
    }
    int status;
    if (waitpid(shell, &status, WNOHANG) == shell) {
        printf("term: shell exited, status 0x%x\n", status);
        fflush(stdout);
        app_quit(app, 0);
    }
}

static void on_poll_shell(void *arg)
{
    int status;
    if (waitpid(shell, &status, WNOHANG) == shell) {
        printf("term: shell exited, status 0x%x\n", status);
        fflush(stdout);
        app_quit(app, 0);
    }
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app) {
        fprintf(stderr, "term: no window server\n");
        return 1;
    }
    struct font *loaded = gfx_font_load("/etc/fonts/mono20.mfnt");
    font = loaded ? loaded : gfx_font_builtin();
    cell_w = font->advance['M'];
    cell_h = font->height;
    win = app_window(app, cols * cell_w, rows * cell_h, "terminal");
    if (!win)
        return 1;
    widget_set_padding(win, 0);
    gui_set_min_size(window_state_of(win)->win, MIN_COLS * cell_w, MIN_ROWS * cell_h);
    canvas = canvas_new(win);
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "key", on_key, NULL);
    widget_connect(canvas, "wheel", on_wheel, NULL);
    widget_connect(win, "resize", on_resize, NULL);
    widget_focus(canvas);
    char slave[32];
    if (openpty(&master, slave, sizeof slave) < 0) {
        perror("term: openpty");
        return 1;
    }
    fcntl(master, F_SETFL, O_NONBLOCK);     /* a full input buffer must not stall the window */
    window_paint(win);
    resize_cells(canvas->w, canvas->h);
    shell = fork();
    if (shell == 0) {
        setpgid(0, 0);
        int s = open(slave, O_RDWR);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        close(s);
        close(master);
        tcsetpgrp(0, getpgrp());
        char *const args[] = { argc > 1 ? argv[1] : "sh", NULL };
        execvp(args[0], args);
        _exit(127);
    }
    printf("term: shell pid %d on %s, %dx%d cells of %dx%d\n", shell, slave, cols, rows, cell_w, cell_h);
    fflush(stdout);
    app_watch_fd(app, master, POLLIN, on_master, NULL);
    app_timer_add(app, 200, 1, on_poll_shell, NULL);
    app_run(app);
    close(master);
    kill(-shell, SIGHUP);
    kill(-shell, SIGKILL);
    waitpid(shell, NULL, 0);
    app_destroy(app);
    return 0;
}
