/* fsinit: mount the filesystems of /etc/fstab at boot and seed a freshly
 * created one with a directory tree. init runs it before the shell.
 *
 *     fsinit [-f fstab] [-v]
 *
 * Each line of the table names a device, a mount point, a filesystem type
 * and a comma separated list of options: nofail (a missing device is not
 * an error), noauto (the entry is skipped), seed=DIR (DIR is copied into
 * the mount point when the mounted filesystem is empty). Mount points that
 * are mounted already are skipped, so the program may run again. The exit
 * status is 1 when a required mount failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>

static int verbose;

static void note(const char *fmt, const char *a, const char *b)
{
    if (!verbose)
        return;
    printf("fsinit: ");
    printf(fmt, a, b);
    printf("\n");
}

/* True when path is a mount point according to /dev/mounts. */
static int is_mounted(const char *path)
{
    FILE *f = fopen("/dev/mounts", "r");
    if (f == NULL)
        return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *space = strchr(line, ' ');
        if (space == NULL)
            continue;
        *space = '\0';
        if (strcmp(line, path) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}

static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL)
        return 0;
    struct dirent *e;
    int empty = 1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            empty = 0;
            break;
        }
    }
    closedir(d);
    return empty;
}

static int copy_file(const char *from, const char *to, mode_t mode)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[16384];
    ssize_t n;
    int r = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t done = 0; done < n; ) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w <= 0) {
                r = -1;
                break;
            }
            done += w;
        }
        if (r < 0)
            break;
    }
    if (n < 0)
        r = -1;
    close(in);
    close(out);
    return r;
}

/* Copy the tree below from into to, which exists. */
static int copy_tree(const char *from, const char *to)
{
    DIR *d = opendir(from);
    if (d == NULL)
        return -1;
    struct dirent *e;
    int r = 0;
    while (r == 0 && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char src[512], dst[512];
        snprintf(src, sizeof src, "%s/%s", from, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
        struct stat st;
        if (stat(src, &st) < 0) {
            r = -1;
        } else if (S_ISDIR(st.st_mode)) {
            if (mkdir(dst, st.st_mode & 07777) < 0 && errno != EEXIST)
                r = -1;
            else
                r = copy_tree(src, dst);
        } else if (S_ISREG(st.st_mode)) {
            r = copy_file(src, dst, st.st_mode);
        }
    }
    closedir(d);
    return r;
}

struct entry {
    char device[64];
    char target[256];
    char type[16];
    char seed[256];
    int nofail, noauto;
};

static int parse_options(struct entry *e, char *options)
{
    for (char *opt = strtok(options, ","); opt != NULL; opt = strtok(NULL, ",")) {
        if (strcmp(opt, "nofail") == 0)
            e->nofail = 1;
        else if (strcmp(opt, "noauto") == 0)
            e->noauto = 1;
        else if (strncmp(opt, "seed=", 5) == 0)
            strlcpy(e->seed, opt + 5, sizeof e->seed);
        else if (strcmp(opt, "defaults") != 0)
            return -1;
    }
    return 0;
}

static int process(const struct entry *e)
{
    if (e->noauto)
        return 0;
    if (is_mounted(e->target)) {
        note("%s is mounted already", e->target, "");
        return 0;
    }
    const char *device = strncmp(e->device, "/dev/", 5) == 0 ? e->device + 5 : e->device;
    if (mount(device, e->target, e->type) < 0) {
        if (e->nofail) {
            note("%s: %s, skipped", e->target, strerror(errno));
            return 0;
        }
        fprintf(stderr, "fsinit: mount %s on %s: %s\n", e->device, e->target, strerror(errno));
        return -1;
    }
    note("mounted %s on %s", e->device, e->target);
    if (e->seed[0] != '\0' && dir_is_empty(e->target)) {
        if (copy_tree(e->seed, e->target) < 0) {
            fprintf(stderr, "fsinit: seeding %s from %s: %s\n", e->target, e->seed, strerror(errno));
            return -1;
        }
        printf("fsinit: seeded %s from %s\n", e->target, e->seed);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *table = "/etc/fstab";
    int opt;
    while ((opt = getopt(argc, argv, "f:v")) != -1) {
        if (opt == 'f')
            table = optarg;
        else if (opt == 'v')
            verbose = 1;
        else {
            fprintf(stderr, "usage: fsinit [-f fstab] [-v]\n");
            return 2;
        }
    }
    FILE *f = fopen(table, "r");
    if (f == NULL) {
        fprintf(stderr, "fsinit: %s: %s\n", table, strerror(errno));
        return 1;
    }
    int status = 0;
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        struct entry e;
        memset(&e, 0, sizeof e);
        char options[256] = "";
        int n = sscanf(line, "%63s %255s %15s %255s", e.device, e.target, e.type, options);
        if (n <= 0)
            continue;
        if (n < 3 || parse_options(&e, options) < 0) {
            fprintf(stderr, "fsinit: %s:%d: invalid entry\n", table, lineno);
            status = 1;
            continue;
        }
        if (process(&e) < 0)
            status = 1;
    }
    fclose(f);
    return status;
}
