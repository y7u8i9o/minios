/* stat: print the attributes of files. A symbolic link is reported
 * itself, with its target; -L reports the file it leads to. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>

static const char *kind(unsigned mode)
{
    if (S_ISDIR(mode)) return "directory";
    if (S_ISREG(mode)) return "regular file";
    if (S_ISLNK(mode)) return "symbolic link";
    if (S_ISCHR(mode)) return "character device";
    return "other";
}

int main(int argc, char **argv)
{
    int follow = 0, first = 1;
    if (first < argc && strcmp(argv[first], "-L") == 0) {
        follow = 1;
        first++;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: stat [-L] file...\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        if ((follow ? stat(argv[i], &st) : lstat(argv[i], &st)) < 0) {
            fprintf(stderr, "stat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        printf("  File: %s", argv[i]);
        if (S_ISLNK(st.st_mode)) {
            char target[1024];
            ssize_t n = readlink(argv[i], target, sizeof target - 1);
            if (n >= 0) {
                target[n] = '\0';
                printf(" -> %s", target);
            }
        }
        struct passwd *pw = getpwuid(st.st_uid);
        char owner[40];
        snprintf(owner, sizeof owner, "%s", pw ? pw->pw_name : "?");
        struct group *gr = getgrgid(st.st_gid);
        printf("\n  Size: %ld\tBlocks: %ld\tIO Block: %ld\t%s\n"
               "Device: %lu\tInode: %lu\tLinks: %u\n  Mode: %04o\tUid: %u (%s)\tGid: %u (%s)\n",
               (long)st.st_size, (long)st.st_blocks, (long)st.st_blksize, kind(st.st_mode),
               (unsigned long)st.st_dev, (unsigned long)st.st_ino, st.st_nlink, st.st_mode & 07777,
               st.st_uid, owner, st.st_gid, gr ? gr->gr_name : "?");
    }
    return status;
}
