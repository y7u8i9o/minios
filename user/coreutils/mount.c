/* mount: mount <type> <source> <target>, or umount <target> via -u. */
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "-u") == 0) {
        if (umount(argv[2]) < 0) {
            fprintf(stderr, "umount: %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "usage: mount <type> <source> <target> | mount -u <target>\n");
        return 2;
    }
    if (mount(argv[2], argv[3], argv[1]) < 0) {
        fprintf(stderr, "mount: %s: %s\n", argv[3], strerror(errno));
        return 1;
    }
    return 0;
}
