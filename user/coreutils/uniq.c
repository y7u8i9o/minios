/* uniq: drop adjacent duplicate lines. -c prefix counts, -d only
 * repeated lines. */
#include <stdio.h>
#include <string.h>
#include <errno.h>

static int counts, dups;

static void flush(const char *line, int n)
{
    if (n == 0 || (dups && n < 2))
        return;
    if (counts)
        printf("%7d %s", n, line);
    else
        fputs(line, stdout);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'c') counts = 1;
            else if (*o == 'd') dups = 1;
            else {
                fprintf(stderr, "uniq: unknown option -%c\n", *o);
                return 2;
            }
        }
    }
    FILE *f = stdin;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "uniq: %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
    }
    char prev[1024], line[1024];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (n && strcmp(line, prev) == 0) {
            n++;
            continue;
        }
        flush(prev, n);
        strcpy(prev, line);
        n = 1;
    }
    flush(prev, n);
    return 0;
}
