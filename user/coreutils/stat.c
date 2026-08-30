/* stat: print the attributes of files. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static const char *kind(unsigned mode)
{
    if (S_ISDIR(mode)) return "directory";
    if (S_ISREG(mode)) return "regular file";
    if (S_ISCHR(mode)) return "character device";
    return "other";
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: stat file...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "stat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        printf("  File: %s\n  Size: %ld\tBlocks: %ld\tIO Block: %ld\t%s\n"
               "Device: %lu\tInode: %lu\tLinks: %u\n  Mode: %04o\n",
               argv[i], (long)st.st_size, (long)st.st_blocks, (long)st.st_blksize, kind(st.st_mode),
               (unsigned long)st.st_dev, (unsigned long)st.st_ino, st.st_nlink, st.st_mode & 07777);
    }
    return status;
}
