/* head: print the first lines (default 10, -n N) of files or stdin. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static long limit = 10;
static int bytes;

static void head(FILE *f)
{
    long count = 0;
    while (count < limit) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        putchar(c);
        if (bytes || c == '\n')
            count++;
    }
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && argv[1][0] == '-' && (argv[1][1] == 'n' || argv[1][1] == 'c')) {
        bytes = argv[1][1] == 'c';
        const char *value = argv[1] + 2;
        first = 2;
        if (!*value && first < argc)
            value = argv[first++];
        char *end;
        limit = strtol(value, &end, 10);
        if (!*value || *end || limit < 0) {
            fprintf(stderr, "head: invalid count\n");
            return 2;
        }
    }
    if (first < argc && !strcmp(argv[first], "--"))
        first++;
    if (first >= argc) {
        head(stdin);
        return ferror(stdin) || ferror(stdout);
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        FILE *f = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "head: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (argc - first > 1)
            printf("==> %s <==\n", argv[i]);
        head(f);
        status |= ferror(f) || ferror(stdout);
        if (f != stdin)
            fclose(f);
    }
    return status;
}
