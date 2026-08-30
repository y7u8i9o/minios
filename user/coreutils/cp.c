#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: cp source dest\n");
        return 1;
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                fprintf(stderr, "cp: write: %s\n", strerror(errno));
                return 1;
            }
            off += w;
        }
    }
    close(in);
    close(out);
    return n < 0 ? 1 : 0;
}
