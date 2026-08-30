/* rm: remove files. -d also removes empty directories. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int status = 0, first = 1, dirs = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        dirs = 1;
        first = 2;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: rm [-d] file...\n");
        return 1;
    }
    for (int i = first; i < argc; i++) {
        struct stat st;
        int r = stat(argv[i], &st);
        if (r == 0)
            r = S_ISDIR(st.st_mode) && dirs ? rmdir(argv[i]) : unlink(argv[i]);
        if (r < 0) {
            fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
