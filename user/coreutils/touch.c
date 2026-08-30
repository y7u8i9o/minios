/* touch: create files that do not exist yet. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        } else {
            close(fd);
        }
    }
    return status;
}
