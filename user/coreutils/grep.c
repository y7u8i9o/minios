/* grep: print lines containing a fixed string. -i ignore case, -n line
 * numbers, -v invert, -c count only, -l names only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int icase, number, invert, count_only, names_only;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int match(const char *line, const char *pat)
{
    if (!icase)
        return strstr(line, pat) != NULL;
    size_t n = strlen(pat);
    for (const char *p = line; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && lower(p[i]) == lower(pat[i]))
            i++;
        if (i == n)
            return 1;
    }
    return 0;
}

static int grep(FILE *f, const char *name, const char *pat, int show_name)
{
    char line[1024];
    int lineno = 0, hits = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n')
            line[--n] = '\0';
        if (match(line, pat) == !invert) {
            hits++;
            if (names_only) {
                printf("%s\n", name);
                return 1;
            }
            if (!count_only) {
                if (show_name)
                    printf("%s:", name);
                if (number)
                    printf("%d:", lineno);
                printf("%s\n", line);
            }
        }
    }
    if (count_only) {
        if (show_name)
            printf("%s:", name);
        printf("%d\n", hits);
    }
    return hits > 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'i': icase = 1; break;
            case 'n': number = 1; break;
            case 'v': invert = 1; break;
            case 'c': count_only = 1; break;
            case 'l': names_only = 1; break;
            default:
                fprintf(stderr, "grep: unknown option -%c\n", *o);
                return 2;
            }
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: grep [-invcl] pattern [file...]\n");
        return 2;
    }
    const char *pat = argv[i++];
    if (i >= argc)
        return grep(stdin, "(standard input)", pat, 0) ? 0 : 1;
    int found = 0, status = 0;
    int show = argc - i > 1;
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "grep: %s: %s\n", argv[i], strerror(errno));
            status = 2;
            continue;
        }
        found |= grep(f, argv[i], pat, show);
        fclose(f);
    }
    return status ? status : found ? 0 : 1;
}
