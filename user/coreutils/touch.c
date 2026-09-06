/* touch: create the files that do not exist and set the modification time
 * of every named file to the current time. -c does not create files. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    int status = 0;
    int create = 1;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-c") == 0) {
        create = 0;
        i++;
    }
    for (; i < argc; i++) {
        if (create) {
            int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
            if (fd < 0) {
                fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                continue;
            }
            close(fd);
        }
        if (utimensat(AT_FDCWD, argv[i], NULL, 0) < 0 && (create || errno != ENOENT)) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
