/* tail: print the last lines of files. -n N selects the count, default 10. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int limit = 10;

static void tail(FILE *f)
{
    char **ring = calloc((size_t)limit, sizeof *ring);
    int n = 0, head = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        int slot = (head + n) % limit;
        if (n == limit) {
            free(ring[head]);
            slot = head;
            head = (head + 1) % limit;
        } else {
            n++;
        }
        ring[slot] = strdup(line);
    }
    for (int i = 0; i < n; i++) {
        fputs(ring[(head + i) % limit], stdout);
        free(ring[(head + i) % limit]);
    }
    free(ring);
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        limit = atoi(argv[2]);
        if (limit <= 0)
            return 0;
        first = 3;
    }
    if (first >= argc) {
        tail(stdin);
        return 0;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "tail: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (argc - first > 1)
            printf("==> %s <==\n", argv[i]);
        tail(f);
        fclose(f);
    }
    return status;
}
