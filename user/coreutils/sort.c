/* sort: sort lines. -r reverse, -n numeric, -u drop duplicates. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int reverse, numeric, unique;
static char **lines;
static size_t nlines, cap;

static void add(const char *s)
{
    if (nlines == cap) {
        cap = cap ? cap * 2 : 64;
        lines = realloc(lines, cap * sizeof *lines);
    }
    lines[nlines++] = strdup(s);
}

static void slurp(FILE *f)
{
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n')
            line[n - 1] = '\0';
        add(line);
    }
}

static int cmp(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r;
    if (numeric) {
        long nx = strtol(x, NULL, 10), ny = strtol(y, NULL, 10);
        r = nx < ny ? -1 : nx > ny ? 1 : strcmp(x, y);
    } else {
        r = strcmp(x, y);
    }
    return reverse ? -r : r;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r') reverse = 1;
            else if (*o == 'n') numeric = 1;
            else if (*o == 'u') unique = 1;
            else {
                fprintf(stderr, "sort: unknown option -%c\n", *o);
                return 2;
            }
        }
    }
    if (i >= argc)
        slurp(stdin);
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "sort: %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
        slurp(f);
        fclose(f);
    }
    qsort(lines, nlines, sizeof *lines, cmp);
    for (size_t k = 0; k < nlines; k++) {
        if (unique && k > 0 && strcmp(lines[k], lines[k - 1]) == 0)
            continue;
        puts(lines[k]);
    }
    return 0;
}
