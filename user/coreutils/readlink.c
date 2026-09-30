/* readlink: print the target of symbolic links.
 *
 *   readlink [-fn] file...
 *
 * Each target is printed on a line of its own. -f prints the absolute
 * path of each file with every symbolic link resolved (realpath) and
 * accepts files that are not links. -n omits the newline. The exit status
 * is 1 when a file is not a symbolic link or cannot be resolved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int canonical = 0, newline = 1, first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'f') {
                canonical = 1;
            } else if (*p == 'n') {
                newline = 0;
            } else {
                fprintf(stderr, "usage: readlink [-fn] file...\n");
                return 2;
            }
        }
    }
    if (first == argc) {
        fprintf(stderr, "usage: readlink [-fn] file...\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        char buf[PATH_MAX];
        if (canonical) {
            if (!realpath(argv[i], buf)) {
                fprintf(stderr, "readlink: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                continue;
            }
        } else {
            ssize_t n = readlink(argv[i], buf, sizeof buf - 1);
            if (n < 0) {
                fprintf(stderr, "readlink: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                continue;
            }
            buf[n] = '\0';
        }
        fputs(buf, stdout);
        if (newline)
            putchar('\n');
    }
    return status;
}
