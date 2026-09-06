/* mount: mount <type> <source> <target>, umount <target> with -u, and the
 * list of mounted filesystems from /dev/mounts without arguments. */
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <errno.h>

static int list_mounts(void)
{
    FILE *f = fopen("/dev/mounts", "r");
    if (f == NULL) {
        fprintf(stderr, "mount: /dev/mounts: %s\n", strerror(errno));
        return 1;
    }
    char path[256], type[32];
    unsigned long total, avail, bs;
    while (fscanf(f, "%255s %31s %lu %lu %lu", path, type, &total, &avail, &bs) == 5)
        printf("%s type %s\n", path, type);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return list_mounts();
    if (argc == 3 && strcmp(argv[1], "-u") == 0) {
        if (umount(argv[2]) < 0) {
            fprintf(stderr, "umount: %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "usage: mount [<type> <source> <target>] | mount -u <target>\n");
        return 2;
    }
    if (mount(argv[2], argv[3], argv[1]) < 0) {
        fprintf(stderr, "mount: %s: %s\n", argv[3], strerror(errno));
        return 1;
    }
    return 0;
}
