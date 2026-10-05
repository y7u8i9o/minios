/* mount: mount [-o options] -t <type> <source> <target>, or with the type
 * as the first operand, umount <target> with -u, and the list of mounted
 * filesystems from /dev/mounts without arguments. The options go to the
 * filesystem (mount(1)). */
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <errno.h>
#include <unistd.h>

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
    const char *options = NULL, *type = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "o:t:")) != -1) {
        if (opt == 'o') {
            options = optarg;
        } else if (opt == 't') {
            type = optarg;
        } else {
            argc = 0;
            break;
        }
    }
    /* The type comes from -t or is the first operand. */
    if (argc && !type && optind < argc)
        type = argv[optind++];
    if (argc == 0 || !type || argc - optind != 2) {
        fprintf(stderr, "usage: mount [-o options] -t <type> <source> <target>\n"
                        "       mount [-o options] <type> <source> <target>\n"
                        "       mount -u <target>\n");
        return 2;
    }
    if (mount_options(argv[optind], argv[optind + 1], type, options) < 0) {
        fprintf(stderr, "mount: %s: %s\n", argv[optind + 1], strerror(errno));
        return 1;
    }
    return 0;
}
