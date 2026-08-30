/* tee: copy standard input to standard output and to files. -a appends. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int i = 1, flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (i < argc && strcmp(argv[i], "-a") == 0) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        i++;
    }
    int fds[16], n = 0;
    for (; i < argc && n < 16; i++) {
        fds[n] = open(argv[i], flags, 0644);
        if (fds[n] < 0) {
            fprintf(stderr, "tee: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        n++;
    }
    char buf[4096];
    long r;
    while ((r = read(0, buf, sizeof buf)) > 0) {
        write(1, buf, (size_t)r);
        for (int k = 0; k < n; k++)
            write(fds[k], buf, (size_t)r);
    }
    for (int k = 0; k < n; k++)
        close(fds[k]);
    return 0;
}
