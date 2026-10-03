/* A seekable pager: input is retained as logical lines, display width is
 * computed separately so SGR attributes never consume screen columns. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <regex.h>
#include <wchar.h>
#include <term.h>

struct text {
    char **lines;
    size_t count, capacity;
};
static int raw_colors, numbers, chop, horizontal;

static int load(struct text *text, FILE *file)
{
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        if (text->count == text->capacity) {
            size_t count = text->capacity ? text->capacity * 2 : 128;
            char **next = realloc(text->lines, count * sizeof *next);
            if (!next) {
                free(line);
                return 1;
            }
            text->lines = next;
            text->capacity = count;
        }
        text->lines[text->count++] = line;
        line = NULL;
        capacity = 0;
    }
    free(line);
    return ferror(file);
}

static size_t sgr_length(const char *text)
{
    if (text[0] != 27 || text[1] != '[')
        return 0;
    size_t n = 2;
    while ((text[n] >= '0' && text[n] <= '9') || text[n] == ';' || text[n] == ':')
        n++;
    return text[n] == 'm' ? n + 1 : 0;
}

static int render_line(const char *line, size_t index, int row, int body, int columns)
{
    int column = 0, skipped = 0;
    printf("\033[%d;1H", row + 1);
    if (numbers) {
        printf("%6lu ", (unsigned long)index + 1);
        column = 7;
    }
    for (size_t i = 0; line[i] && line[i] != '\n';) {
        size_t sgr = sgr_length(line + i);
        if (sgr) {
            if (raw_colors)
                fwrite(line + i, 1, sgr, stdout);
            i += sgr;
            continue;
        }
        mbstate_t state = {0};
        wchar_t wc;
        size_t n = mbrtowc(&wc, line + i, strlen(line + i), &state);
        int width = 1;
        if (n == (size_t)-1 || n == (size_t)-2 || !n) {
            n = 1;
            wc = '?';
        } else {
            width = wcwidth(wc);
            if (width < 0)
                width = 1;
        }
        if (wc == '\t')
            width = 8 - (column % 8);
        if (chop && skipped < horizontal) {
            skipped += width;
            i += n;
            continue;
        }
        if (column + width > columns) {
            if (chop) {
                i += n;
                continue;
            }
            row++;
            if (row >= body)
                break;
            printf("\033[%d;1H", row + 1);
            column = 0;
        }
        if (wc == '\t') {
            for (int j = 0; j < width; j++)
                putchar(' ');
        } else if (wc < 32 || wc == 127) {
            putchar('?');
        } else {
            fwrite(line + i, 1, n, stdout);
        }
        column += width;
        i += n;
    }
    fputs("\033[0m", stdout);
    return row + 1;
}

static int render(const struct text *text, size_t top, int rows, int columns, const char *name,
                  const char *message)
{
    fputs("\033[H\033[2J", stdout);
    int row = 0, body = rows - 1;
    size_t last = top;
    for (; last < text->count && row < body; last++)
        row = render_line(text->lines[last], last, row, body, columns);
    unsigned percent = text->count ? (unsigned)(last * 100 / text->count) : 100;
    printf("\033[%d;1H\033[7m", rows);
    char status[512];
    snprintf(status, sizeof status, " %s  %lu/%lu  %u%%  %s", name,
             (unsigned long)(text->count ? top + 1 : 0), (unsigned long)text->count, percent,
             message ? message : "q quit / search");
    printf("%.*s\033[0m", columns - 1, status);
    fflush(stdout);
    return (int)(last - top);
}

static int input_search(int tty, int rows, char direction, char *query, size_t size)
{
    size_t used = 0;
    printf("\033[%d;1H\033[K%c", rows, direction);
    fflush(stdout);
    for (;;) {
        unsigned char c;
        if (read(tty, &c, 1) != 1 || c == 27 || c == 3)
            return -1;
        if (c == '\n' || c == '\r')
            break;
        if ((c == 8 || c == 127) && used) {
            used--;
            printf("\b \b");
        } else if (c >= 32 && used + 1 < size) {
            query[used++] = (char)c;
            putchar(c);
        }
        fflush(stdout);
    }
    query[used] = 0;
    return (int)used;
}

static char *plain_line(const char *line)
{
    char *out = malloc(strlen(line) + 1);
    if (!out)
        return NULL;
    size_t n = 0;
    while (*line) {
        size_t sgr = sgr_length(line);
        if (sgr)
            line += sgr;
        else
            out[n++] = *line++;
    }
    out[n] = 0;
    return out;
}

static int search(const struct text *text, size_t *top, const regex_t *regex, int direction)
{
    long at = (long)*top + direction;
    for (; at >= 0 && (size_t)at < text->count; at += direction) {
        char *line = plain_line(text->lines[at]);
        if (!line)
            return 0;
        int matched = regexec(regex, line, 0, NULL, 0) == 0;
        free(line);
        if (matched) {
            *top = (size_t)at;
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'R')
                raw_colors = 1;
            else if (*p == 'N')
                numbers = 1;
            else if (*p == 'S')
                chop = 1;
            else {
                fprintf(stderr, "usage: less [-RNS] [file...]\n");
                return 2;
            }
        }
    }
    struct text text = {0};
    int status = 0;
    const char *name = first < argc ? argv[first] : "standard input";
    if (first == argc)
        status = load(&text, stdin);
    for (int i = first; i < argc; i++) {
        FILE *file = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!file) {
            fprintf(stderr, "less: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        status |= load(&text, file);
        if (file != stdin)
            fclose(file);
    }
    int tty = -1;
    struct termios saved;
    /* The shell opens terminal descriptors read/write. When stdin is a
     * pipe, stdout still identifies this window's pty (or the console).
     * Opening /dev/console here would steal input from another terminal. */
    if (isatty(1))
        tty = isatty(0) ? 0 : dup(1);
    if (tty < 0 || tcgetattr(tty, &saved) < 0) {
        for (size_t i = 0; i < text.count; i++)
            fputs(text.lines[i], stdout);
    } else {
        struct termios raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        if (tcsetattr(tty, TCSANOW, &raw) < 0) {
            status = 1;
        } else {
            size_t top = 0;
            regex_t regex;
            int have_regex = 0, direction = 1;
            char query[256] = "", message[128] = "";
            unsigned percentage = 0;
            int have_percentage = 0;
            for (;;) {
                struct winsize ws;
                int rows = ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 ? ws.ws_row : 24;
                int page = render(&text, top, rows, term_columns(1), name, *message ? message : NULL);
                if (page < 1)
                    page = 1;
                unsigned char c;
                if (read(tty, &c, 1) != 1 || c == 'q' || c == 3)
                    break;
                message[0] = 0;
                if (c >= '0' && c <= '9') {
                    percentage = (percentage * 10 + c - '0') % 1000;
                    have_percentage = 1;
                    continue;
                }
                if (c == ' ' || c == 'f')
                    top += (size_t)page;
                else if (c == '\n' || c == '\r' || c == 'j')
                    top++;
                else if (c == 'b')
                    top = top > (size_t)page ? top - (size_t)page : 0;
                else if (c == 'k' && top)
                    top--;
                else if (c == 'g')
                    top = 0;
                else if (c == 'G')
                    top = text.count > (size_t)(rows - 1) ? text.count - (size_t)(rows - 1) : 0;
                else if (c == '%' && have_percentage)
                    top = text.count * (percentage > 100 ? 100 : percentage) / 100;
                else if (c == '>' && chop)
                    horizontal += 8;
                else if (c == '<' && chop)
                    horizontal = horizontal > 8 ? horizontal - 8 : 0;
                else if (c == '/' || c == '?') {
                    char next[256];
                    int length = input_search(tty, rows, (char)c, next, sizeof next);
                    if (length >= 0) {
                        if (length)
                            strcpy(query, next);
                        regex_t compiled;
                        int error = regcomp(&compiled, query, REG_EXTENDED);
                        if (error) {
                            regerror(error, &compiled, message, sizeof message);
                        } else {
                            if (have_regex)
                                regfree(&regex);
                            regex = compiled;
                            have_regex = 1;
                            direction = c == '/' ? 1 : -1;
                            c = 'n';
                        }
                    }
                }
                if ((c == 'n' || c == 'N') && have_regex &&
                    !search(&text, &top, &regex, c == 'N' ? -direction : direction))
                    strcpy(message, "pattern not found");
                if (c == 'h')
                    strcpy(message, "Space/b page j/k line g/G ends /? search n/N repeat 50% position");
                if (top >= text.count)
                    top = text.count ? text.count - 1 : 0;
                percentage = 0;
                have_percentage = 0;
            }
            if (have_regex)
                regfree(&regex);
            fputs("\033[0m\033[H\033[2J", stdout);
            fflush(stdout);
            tcsetattr(tty, TCSANOW, &saved);
        }
    }
    if (tty > 0)
        close(tty);
    for (size_t i = 0; i < text.count; i++)
        free(text.lines[i]);
    free(text.lines);
    return status;
}
