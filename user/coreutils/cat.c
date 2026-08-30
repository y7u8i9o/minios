/* cat: concatenate files (or standard input) to standard output. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int copy(int fd)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(1, buf + off, (size_t)(n - off));
            if (w < 0)
                return -1;
            off += w;
        }
    }
}

int main(int argc, char **argv)
{
    int status = 0;
    if (argc == 1)
        return copy(0) < 0 ? 1 : 0;
    for (int i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") == 0 ? 0 : open(argv[i], O_RDONLY);
        if (fd < 0 || copy(fd) < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        if (fd > 0)
            close(fd);
    }
    return status;
}
