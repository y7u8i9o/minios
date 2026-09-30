/* rm: remove files. -d also removes empty directories; -f ignores names
 * that do not exist. A symbolic link is removed itself, never the file it
 * leads to. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int status = 0, first = 1, dirs = 0, force = 0;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'd') {
                dirs = 1;
            } else if (*p == 'f') {
                force = 1;
            } else {
                fprintf(stderr, "usage: rm [-df] file...\n");
                return 1;
            }
        }
    }
    if (first >= argc && !force) {
        fprintf(stderr, "usage: rm [-df] file...\n");
        return 1;
    }
    for (int i = first; i < argc; i++) {
        struct stat st;
        int r = lstat(argv[i], &st);
        if (r == 0)
            r = S_ISDIR(st.st_mode) && dirs ? rmdir(argv[i]) : unlink(argv[i]);
        if (r < 0 && !(force && errno == ENOENT)) {
            fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
