#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* mkdir -p: every missing component of the path, without an error for the
 * ones that exist. */
static int mkdir_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(path, 0755) < 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    return mkdir(path, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

int main(int argc, char **argv)
{
    int status = 0, parents = 0, i = 1;
    if (argc > 1 && strcmp(argv[1], "-p") == 0) {
        parents = 1;
        i = 2;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: mkdir [-p] dir...\n");
        return 1;
    }
    for (; i < argc; i++) {
        int r = parents ? mkdir_parents(argv[i]) : mkdir(argv[i], 0755);
        if (r < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
