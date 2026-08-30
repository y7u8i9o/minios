/* edit: a small screen editor over the console in raw keyboard mode.
 *
 * Keys: arrows, Home, End, PageUp, PageDown move; typing inserts;
 * Backspace and Delete erase; Enter splits a line; control S saves;
 * control Q quits (twice when there are unsaved changes); control L
 * redraws. The last screen row shows the file name, size and cursor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>

struct line {
    char *text;
    int len, cap;
};

static struct line *lines;
static int nlines, cap_lines;
static int cx, cy;              /* cursor: column, line */
static int rowoff, coloff;      /* first visible line and column */
static int rows = 24, cols = 80;
static int dirty;
static char filename[256];
static char status[128];
static struct termios saved;

/* ---- buffer ---- */

static void line_insert(int at, const char *s, int n)
{
    if (nlines == cap_lines) {
        cap_lines = cap_lines ? cap_lines * 2 : 64;
        lines = realloc(lines, (size_t)cap_lines * sizeof *lines);
    }
    memmove(&lines[at + 1], &lines[at], (size_t)(nlines - at) * sizeof *lines);
    lines[at].cap = n + 16;
    lines[at].text = malloc((size_t)lines[at].cap);
    memcpy(lines[at].text, s, (size_t)n);
    lines[at].len = n;
    nlines++;
}

static void line_delete(int at)
{
    free(lines[at].text);
    memmove(&lines[at], &lines[at + 1], (size_t)(nlines - at - 1) * sizeof *lines);
    nlines--;
}

static void line_insert_char(struct line *l, int at, char c)
{
    if (l->len + 1 > l->cap) {
        l->cap = l->cap * 2 + 16;
        l->text = realloc(l->text, (size_t)l->cap);
    }
    memmove(l->text + at + 1, l->text + at, (size_t)(l->len - at));
    l->text[at] = c;
    l->len++;
}

static void load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        line_insert(0, "", 0);
        snprintf(status, sizeof status, errno == ENOENT ? "new file" : "cannot read: %s", strerror(errno));
        return;
    }
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) {
        int n = (int)strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            n--;
        line_insert(nlines, buf, n);
    }
    fclose(f);
    if (nlines == 0)
        line_insert(0, "", 0);
    snprintf(status, sizeof status, "%d lines read", nlines);
}

static void save(void)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        snprintf(status, sizeof status, "cannot write %s: %s", filename, strerror(errno));
        return;
    }
    long bytes = 0;
    for (int i = 0; i < nlines; i++) {
        fwrite(lines[i].text, 1, (size_t)lines[i].len, f);
        fputc('\n', f);
        bytes += lines[i].len + 1;
    }
    if (fclose(f) != 0) {
        snprintf(status, sizeof status, "write error: %s", strerror(errno));
        return;
    }
    dirty = 0;
    snprintf(status, sizeof status, "%ld bytes written to %s", bytes, filename);
}

/* ---- screen ---- */

static void out(const char *s)
{
    write(1, s, strlen(s));
}

static void draw(void)
{
    char buf[512];
    if (cy < rowoff)
        rowoff = cy;
    if (cy >= rowoff + rows - 1)
        rowoff = cy - (rows - 1) + 1;
    if (cx < coloff)
        coloff = cx;
    if (cx >= coloff + cols)
        coloff = cx - cols + 1;
    out("\033[H");
    for (int r = 0; r < rows - 1; r++) {
        int i = rowoff + r;
        snprintf(buf, sizeof buf, "\033[%d;1H\033[K", r + 1);
        out(buf);
        if (i >= nlines) {
            out("~");
            continue;
        }
        int len = lines[i].len - coloff;
        if (len > 0)
            write(1, lines[i].text + coloff, (size_t)(len > cols ? cols : len));
    }
    snprintf(buf, sizeof buf, "\033[%d;1H\033[K%s%s  %d lines  L%d C%d  %s", rows,
             filename, dirty ? " [modified]" : "", nlines, cy + 1, cx + 1, status);
    out(buf);
    snprintf(buf, sizeof buf, "\033[%d;%dH", cy - rowoff + 1, cx - coloff + 1);
    out(buf);
}

/* ---- input ---- */

enum key { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL };

static int read_byte(void)
{
    unsigned char c;
    ssize_t n;
    while ((n = read(0, &c, 1)) < 0 && errno == EINTR)
        ;
    return n == 1 ? c : -1;
}

static int read_key(void)
{
    int c = read_byte();
    if (c != 27)
        return c;
    int c1 = read_byte();
    if (c1 != '[')
        return 27;
    int c2 = read_byte();
    switch (c2) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '3': case '5': case '6': {
        int c3 = read_byte();
        if (c3 != '~')
            return 27;
        return c2 == '3' ? K_DEL : c2 == '5' ? K_PGUP : K_PGDN;
    }
    }
    return 27;
}

static void clamp(void)
{
    if (cy >= nlines)
        cy = nlines - 1;
    if (cx > lines[cy].len)
        cx = lines[cy].len;
}

static int handle(int key)
{
    static int quit_pending;
    struct line *l = &lines[cy];
    if (key != 0x11)
        quit_pending = 0;
    switch (key) {
    case 0x11:  /* control Q */
        if (dirty && !quit_pending) {
            quit_pending = 1;
            snprintf(status, sizeof status, "unsaved changes, control Q again to quit");
            return 1;
        }
        return 0;
    case 0x13:  /* control S */
        save();
        return 1;
    case 0x0c:  /* control L */
        out("\033[2J");
        return 1;
    case K_UP: if (cy > 0) cy--; clamp(); return 1;
    case K_DOWN: if (cy + 1 < nlines) cy++; clamp(); return 1;
    case K_LEFT:
        if (cx > 0) cx--;
        else if (cy > 0) { cy--; cx = lines[cy].len; }
        return 1;
    case K_RIGHT:
        if (cx < l->len) cx++;
        else if (cy + 1 < nlines) { cy++; cx = 0; }
        return 1;
    case K_HOME: cx = 0; return 1;
    case K_END: cx = l->len; return 1;
    case K_PGUP: cy -= rows - 2; if (cy < 0) cy = 0; clamp(); return 1;
    case K_PGDN: cy += rows - 2; clamp(); return 1;
    case '\r':
    case '\n':
        line_insert(cy + 1, l->text + cx, l->len - cx);
        lines[cy].len = cx;
        cy++;
        cx = 0;
        dirty = 1;
        return 1;
    case 127:
    case '\b':
        if (cx > 0) {
            memmove(l->text + cx - 1, l->text + cx, (size_t)(l->len - cx));
            l->len--;
            cx--;
            dirty = 1;
        } else if (cy > 0) {
            struct line *prev = &lines[cy - 1];
            int at = prev->len;
            for (int i = 0; i < l->len; i++)
                line_insert_char(prev, prev->len, l->text[i]);
            line_delete(cy);
            cy--;
            cx = at;
            dirty = 1;
        }
        return 1;
    case K_DEL:
        if (cx < l->len) {
            memmove(l->text + cx, l->text + cx + 1, (size_t)(l->len - cx - 1));
            l->len--;
            dirty = 1;
        } else if (cy + 1 < nlines) {
            struct line *next = &lines[cy + 1];
            for (int i = 0; i < next->len; i++)
                line_insert_char(l, l->len, next->text[i]);
            line_delete(cy + 1);
            dirty = 1;
        }
        return 1;
    }
    if (key == '\t' || (key >= 32 && key < 127)) {
        line_insert_char(l, cx, (char)key);
        cx++;
        dirty = 1;
    }
    return 1;
}

static void restore_terminal(void)
{
    tcsetattr(0, TCSANOW, &saved);
    char buf[32];
    snprintf(buf, sizeof buf, "\033[%d;1H\033[K", rows);
    out(buf);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: edit file\n");
        return 2;
    }
    strlcpy(filename, argv[1], sizeof filename);
    load(filename);
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 2) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    if (tcgetattr(0, &saved) < 0) {
        perror("edit: tcgetattr");
        return 1;
    }
    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw);
    atexit(restore_terminal);
    out("\033[2J");
    for (;;) {
        draw();
        int key = read_key();
        if (key < 0)
            break;
        if (!handle(key))
            break;
    }
    return 0;
}
