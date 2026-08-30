/* ps: print the process table from /dev/proc. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(void)
{
    int fd = open("/dev/proc", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ps: /dev/proc: %s\n", strerror(errno));
        return 1;
    }
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        write(1, buf, (size_t)n);
    close(fd);
    return 0;
}
