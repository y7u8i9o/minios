/* hexdump: offset, 16 bytes in hex and their printable form per line. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static void dump(int fd)
{
    unsigned char buf[16];
    unsigned long off = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        printf("%08lx ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n)
                printf(" %02x", buf[i]);
            else
                printf("   ");
            if (i == 7)
                putchar(' ');
        }
        printf("  |");
        for (int i = 0; i < n; i++)
            putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
        printf("|\n");
        off += (unsigned long)n;
    }
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        dump(0);
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "hexdump: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        dump(fd);
        close(fd);
    }
    return status;
}
