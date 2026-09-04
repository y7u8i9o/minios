/* pager: a small full-screen text pager with forward/backward movement
 * and searching. It copies input into memory so pipes remain seekable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>

struct text {
    char **line;
    int count, cap;
};

static int add_line(struct text *t, const char *s)
{
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 128;
        char **p = realloc(t->line, (size_t)cap * sizeof *p);
        if (!p)
            return -1;
        t->line = p;
        t->cap = cap;
    }
    t->line[t->count] = strdup(s);
    if (!t->line[t->count])
        return -1;
    t->count++;
    return 0;
}

static int load_file(struct text *t, FILE *f)
{
    char buf[1024];
    while (fgets(buf, sizeof buf, f))
        if (add_line(t, buf) < 0)
            return -1;
    return ferror(f) ? -1 : 0;
}

static int contains(const char *s, const char *pat)
{
    return pat[0] && strstr(s, pat) != NULL;
}

static int read_search(int tty, char *buf, size_t size)
{
    size_t n = 0;
    printf("\033[2K\r/");
    fflush(stdout);
    for (;;) {
        char c;
        if (read(tty, &c, 1) != 1)
            return -1;
        if (c == '\r' || c == '\n')
            break;
        if ((c == 127 || c == '\b') && n) {
            n--;
            printf("\b \b");
            fflush(stdout);
        } else if (c >= 32 && c < 127 && n + 1 < size) {
            buf[n++] = c;
            putchar(c);
            fflush(stdout);
        }
    }
    buf[n] = '\0';
    return n ? 0 : -1;
}

static void render(const struct text *t, int top, int rows, const char *name)
{
    printf("\033[H\033[2J");
    int body = rows > 2 ? rows - 1 : 1;
    for (int i = 0; i < body; i++) {
        int at = top + i;
        if (at < t->count) {
            fputs(t->line[at], stdout);
            size_t n = strlen(t->line[at]);
            if (!n || t->line[at][n - 1] != '\n')
                putchar('\n');
        } else {
            putchar('\n');
        }
    }
    int percent = t->count ? (top + body) * 100 / t->count : 100;
    if (percent > 100)
        percent = 100;
    printf("\033[7m %s  line %d/%d  %d%%  (h for help) \033[0m",
           name, t->count ? top + 1 : 0, t->count, percent);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct text t = {0};
    const char *name = "standard input";
    int first = 1;
    if (argc > 1) {
        name = argv[1];
        for (int i = 1; i < argc; i++) {
            FILE *f = fopen(argv[i], "r");
            if (!f) {
                fprintf(stderr, "pager: %s: %s\n", argv[i], strerror(errno));
                return 1;
            }
            if (argc > 2) {
                char heading[256];
                snprintf(heading, sizeof heading, "%s==> %s <==\n", first ? "" : "\n", argv[i]);
                add_line(&t, heading);
            }
            first = 0;
            if (load_file(&t, f) < 0) {
                fprintf(stderr, "pager: %s: read error\n", argv[i]);
                fclose(f);
                return 1;
            }
            fclose(f);
        }
    } else if (load_file(&t, stdin) < 0) {
        fprintf(stderr, "pager: read error\n");
        return 1;
    }

    if (!isatty(1)) {
        for (int i = 0; i < t.count; i++)
            fputs(t.line[i], stdout);
        return 0;
    }

    int tty = isatty(0) ? 0 : open("/dev/console", O_RDONLY);
    struct termios saved, raw;
    if (tty < 0 || tcgetattr(tty, &saved) < 0) {
        for (int i = 0; i < t.count; i++)
            fputs(t.line[i], stdout);
        return 0;
    }
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(tty, TCSANOW, &raw);

    struct winsize ws;
    int rows = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row)
        rows = ws.ws_row;
    int page = rows > 2 ? rows - 1 : 1;
    int top = 0;
    char search[128] = "";
    for (;;) {
        render(&t, top, rows, name);
        char c;
        if (read(tty, &c, 1) != 1 || c == 'q')
            break;
        if (c == ' ' || c == 'f')
            top += page;
        else if (c == '\r' || c == '\n' || c == 'j')
            top++;
        else if (c == 'b')
            top -= page;
        else if (c == 'k')
            top--;
        else if (c == 'g')
            top = 0;
        else if (c == 'G')
            top = t.count - page;
        else if (c == '/' && read_search(tty, search, sizeof search) == 0)
            c = 'n';
        if (c == 'n' && search[0]) {
            for (int i = top + 1; i < t.count; i++)
                if (contains(t.line[i], search)) {
                    top = i;
                    break;
                }
        }
        if (c == 'h') {
            printf("\033[H\033[2JSpace/f page down   b page up   Enter/j line down   k line up\n"
                   "g first line        G last page  / search            n next match\n"
                   "q quit\n\nPress any key to return.");
            fflush(stdout);
            read(tty, &c, 1);
        }
        int max = t.count > page ? t.count - page : 0;
        if (top < 0)
            top = 0;
        if (top > max)
            top = max;
    }
    tcsetattr(tty, TCSANOW, &saved);
    printf("\033[H\033[2J");
    if (tty != 0)
        close(tty);
    return 0;
}
