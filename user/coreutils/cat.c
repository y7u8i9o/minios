/* cat: concatenate files (or standard input) to standard output. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int number, nonblank, squeeze, visible;
static unsigned long line_number;
static int at_start = 1, blank_run;

static void show_byte(unsigned char c)
{
    if (!visible) {
        putchar(c);
        return;
    }
    if (c == '\n') {
        fputs("$\n", stdout);
    } else if (c == '\t') {
        fputs("^I", stdout);
    } else {
        if (c >= 128) {
            fputs("M-", stdout);
            c -= 128;
        }
        if (c < 32 || c == 127) {
            putchar('^');
            putchar(c == 127 ? '?' : c + 64);
        } else {
            putchar(c);
        }
    }
}

static int copy(int fd)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        if (number || nonblank || squeeze || visible) {
            for (ssize_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)buf[i];
                if (at_start) {
                    if (c == '\n') {
                        if (squeeze && blank_run)
                            continue;
                        blank_run = 1;
                    } else {
                        blank_run = 0;
                    }
                    if ((nonblank && c != '\n') || (number && !nonblank))
                        printf("%6lu\t", ++line_number);
                }
                show_byte(c);
                at_start = c == '\n';
            }
            if (ferror(stdout))
                return -1;
            continue;
        }
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(1, buf + off, (size_t)(n - off));
            if (w < 0)
                return -1;
            off += w;
        }
    }
}

int main(int argc, char **argv)
{
    int status = 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        for (const char *p = argv[first] + 1; *p; p++) {
            switch (*p) {
            case 'n':
                number = 1;
                break;
            case 'b':
                nonblank = 1;
                break;
            case 's':
                squeeze = 1;
                break;
            case 'A':
                visible = 1;
                break;
            default:
                fprintf(stderr, "cat: unknown option -%c\n", *p);
                return 2;
            }
        }
    }
    if (first == argc)
        return copy(0) < 0 ? 1 : 0;
    for (int i = first; i < argc; i++) {
        int fd = strcmp(argv[i], "-") == 0 ? 0 : open(argv[i], O_RDONLY);
        if (fd < 0 || copy(fd) < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        if (fd > 0)
            close(fd);
    }
    return status;
}
