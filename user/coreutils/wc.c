/* wc: count lines, words and bytes. -l, -w and -c select single counts. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int only;   /* 0 all, or one of 'l', 'w', 'c' */

static void report(long lines, long words, long bytes, const char *name)
{
    if (only == 'l')
        printf("%7ld", lines);
    else if (only == 'w')
        printf("%7ld", words);
    else if (only == 'c')
        printf("%7ld", bytes);
    else
        printf("%7ld %7ld %7ld", lines, words, bytes);
    if (name)
        printf(" %s", name);
    printf("\n");
}

static void count(int fd, const char *name, long *tl, long *tw, long *tb)
{
    char buf[4096];
    long lines = 0, words = 0, bytes = 0;
    int inword = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        bytes += n;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                inword = 0;
            } else if (!inword) {
                inword = 1;
                words++;
            }
        }
    }
    report(lines, words, bytes, name);
    *tl += lines;
    *tw += words;
    *tb += bytes;
}

int main(int argc, char **argv)
{
    long tl = 0, tw = 0, tb = 0;
    int first = 1;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] && !argv[1][2] && strchr("lwc", argv[1][1])) {
        only = argv[1][1];
        first = 2;
    }
    if (first >= argc) {
        count(0, NULL, &tl, &tw, &tb);
        return 0;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        count(fd, argv[i], &tl, &tw, &tb);
        close(fd);
    }
    if (argc - first > 1)
        report(tl, tw, tb, "total");
    return status;
}
