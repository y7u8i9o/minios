/* head: print the first lines (default 10, -n N) of files or stdin. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int limit = 10;

static void head(FILE *f)
{
    char line[1024];
    for (int n = 0; n < limit && fgets(line, sizeof line, f); n++)
        fputs(line, stdout);
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        limit = atoi(argv[2]);
        first = 3;
    }
    if (first >= argc) {
        head(stdin);
        return 0;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "head: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (argc - first > 1)
            printf("==> %s <==\n", argv[i]);
        head(f);
        fclose(f);
    }
    return status;
}
