/* fsinit: mount the filesystems of /etc/fstab at boot and seed a freshly
 * created one with a directory tree. init runs it before the shell.
 *
 *     fsinit [-f fstab] [-v]
 *     fsinit -m DIR
 *
 * Each line of the table names a device, a mount point, a filesystem type
 * and a comma separated list of options: nofail (a missing device is not
 * an error), noauto (the entry is skipped), seed=DIR (DIR is copied into
 * the mount point when the mounted filesystem is empty), homes (the volume
 * contains the home directories, see below). Any other option,
 * such as uid=, gid= or umask= of FAT, is passed to the filesystem. Mount
 * points that are mounted already are skipped, so the program may run
 * again. The exit status is 1 when a required mount failed.
 *
 * On a volume with the homes option fsinit converts the layout of the
 * single user, in which the volume itself was the home directory, into the
 * layout of docs/design/users.md: the old contents move to user/, the
 * package prefix .local remains and gains the account databases, and the
 * marker .layout records the conversion. It then makes the missing homes
 * of the accounts of /etc/passwd that lie on the volume. -m DIR converts
 * the directory DIR alone, for the fs_migrate test.
 *
 * After the table fsinit mounts the folders that the host shares through
 * virtio-9p at /mnt/TAG. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <pwd.h>
#include <minios/account.h>

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

struct entry {
    char device[64];
    char target[256];
    char type[16];
    char seed[256];
    char fsopts[128];               /* options for the filesystem itself */
    int nofail, noauto, homes;
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
        else if (strcmp(opt, "homes") == 0)
            e->homes = 1;
        else if (strcmp(opt, "defaults") != 0) {
            size_t used = strlen(e->fsopts);
            if (used + strlen(opt) + 2 > sizeof e->fsopts)
                return -1;
            snprintf(e->fsopts + used, sizeof e->fsopts - used, "%s%s", used ? "," : "", opt);
        }
    }
    return 0;
}

#define LAYOUT_MARKER ".layout"
#define LAYOUT_VERSION "2\n"
#define SEED_LOCAL "/usr/share/skel/home/.local"
#define FIRST_USER 1000

/* Convert dir from the single user layout, in which dir was the home of
 * the one user, uid FIRST_USER. Every entry but the package prefix .local
 * and lost+found moves into user/, through a temporary name, since the old
 * home may contain an entry named user itself. Returns 1 after a conversion,
 * 0 when there was nothing to do, -1 on an error. */
static int migrate(const char *dir)
{
    char path[512], from[512], to[512];
    snprintf(path, sizeof path, "%s/" LAYOUT_MARKER, dir);
    if (access(path, F_OK) == 0)
        return 0;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s/.migrate-user", dir);
    if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
        return -1;
    /* The directory changes with every move, so it is read again from the
     * start for the next entry. */
    int moved = 0;
    for (;;) {
        DIR *d = opendir(dir);
        if (d == NULL)
            return -1;
        struct dirent *e;
        char name[256] = "";
        while ((e = readdir(d)) != NULL) {
            const char *n = e->d_name;
            if (strcmp(n, ".") && strcmp(n, "..") && strcmp(n, ".local") && strcmp(n, "lost+found") &&
                strcmp(n, ".migrate-user")) {
                snprintf(name, sizeof name, "%s", n);
                break;
            }
        }
        closedir(d);
        if (!name[0])
            break;
        snprintf(from, sizeof from, "%s/%s", dir, name);
        snprintf(to, sizeof to, "%s/%s", tmp, name);
        if (rename(from, to) < 0)
            return -1;
        moved++;
    }
    snprintf(to, sizeof to, "%s/user", dir);
    /* With nothing to move the home is made from the skeleton later. */
    if (moved == 0) {
        rmdir(tmp);
    } else if (rename(tmp, to) < 0 || chmod(to, 0700) < 0 ||
               account_chown_tree(to, FIRST_USER, FIRST_USER) < 0) {
        return -1;
    }
    /* The package prefix belongs to root, and it contains the account
     * databases from now on. */
    snprintf(path, sizeof path, "%s/.local", dir);
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return -1;
    if (account_chown_tree(path, 0, 0) < 0 || chmod(path, 0755) < 0)
        return -1;
    snprintf(path, sizeof path, "%s/.local/etc", dir);
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return -1;
    snprintf(from, sizeof from, SEED_LOCAL "/etc");
    if (account_copy_tree(from, path) < 0)
        return -1;
    snprintf(path, sizeof path, "%s/" LAYOUT_MARKER, dir);
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -1;
    fputs(LAYOUT_VERSION, f);
    fclose(f);
    printf("fsinit: moved %d entries of the single user home into %s/user\n", moved, dir);
    return 1;
}

#define WHEEL_GID 10

/* Add the group wheel to the group file of a data volume that has none,
 * with the account FIRST_USER as its member. Volumes made before U5
 * (docs/design/users.md) have no wheel, and their first account therefore
 * cannot use sudo or doas. A gid of 10 that another group uses is left
 * alone. Returns 1 after adding the group, 0 when there was nothing to do,
 * -1 on an error. */
static int add_wheel(const char *dir)
{
    char path[512], line[256];
    snprintf(path, sizeof path, "%s/.local/etc/group", dir);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return errno == ENOENT ? 0 : -1;
    bool found = false;
    while (fgets(line, sizeof line, f) != NULL) {
        char *name_end = strchr(line, ':');
        char *gid = name_end ? strchr(name_end + 1, ':') : NULL;
        if (name_end && (size_t)(name_end - line) == 5 && strncmp(line, "wheel", 5) == 0)
            found = true;
        if (gid && atoi(gid + 1) == WHEEL_GID)
            found = true;
    }
    fclose(f);
    if (found)
        return 0;
    struct passwd *pw = getpwuid(FIRST_USER);
    snprintf(line, sizeof line, "wheel:x:%d:%s", WHEEL_GID, pw ? pw->pw_name : "");
    if (account_replace(path, "wheel", line) < 0)
        return -1;
    printf("fsinit: added the group wheel with %s to %s\n", pw ? pw->pw_name : "no member", path);
    return 1;
}

/* Make the missing homes of the accounts whose home lies below dir. */
static int make_homes(const char *dir)
{
    size_t len = strlen(dir);
    setpwent();
    struct passwd *pw;
    int r = 0;
    char home[256];
    unsigned uid, gid;
    while ((pw = getpwent()) != NULL) {
        if (strncmp(pw->pw_dir, dir, len) != 0 || pw->pw_dir[len] != '/')
            continue;
        snprintf(home, sizeof home, "%s", pw->pw_dir);
        uid = pw->pw_uid;
        gid = pw->pw_gid;
        if (access(home, F_OK) == 0)
            continue;
        if (account_make_home(home, uid, gid) < 0) {
            fprintf(stderr, "fsinit: home %s: %s\n", home, strerror(errno));
            r = -1;
        } else {
            note("made the home %s", home, "");
        }
    }
    endpwent();
    return r;
}

/* The device of a partition named by PARTUUID=GUID, from the listing of
 * /dev/partitions ("name disk partuuid typeuuid bytes"). Returns 0 with
 * the name in out, or -1 with errno ENODEV. */
static int partuuid_device(const char *guid, char *out, size_t size)
{
    FILE *f = fopen("/dev/partitions", "r");
    char line[256], name[32], disk[32], uuid[64];
    int found = 0;
    while (f && !found && fgets(line, sizeof line, f))
        if (sscanf(line, "%31s %31s %63s", name, disk, uuid) == 3 && strcasecmp(uuid, guid) == 0) {
            snprintf(out, size, "%s", name);
            found = 1;
        }
    if (f)
        fclose(f);
    if (!found)
        errno = ENODEV;
    return found ? 0 : -1;
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
    char named[32];
    if (strncmp(device, "PARTUUID=", 9) == 0) {
        if (partuuid_device(device + 9, named, sizeof named) < 0) {
            if (e->nofail) {
                note("%s: no partition %s, skipped", e->target, device + 9);
                return 0;
            }
            fprintf(stderr, "fsinit: %s: no partition %s\n", e->target, device + 9);
            return -1;
        }
        device = named;
    }
    if (mount_options(device, e->target, e->type, e->fsopts) < 0) {
        if (e->nofail) {
            note("%s: %s, skipped", e->target, strerror(errno));
            return 0;
        }
        fprintf(stderr, "fsinit: mount %s on %s: %s\n", e->device, e->target, strerror(errno));
        return -1;
    }
    note("mounted %s on %s", e->device, e->target);
    if (e->seed[0] != '\0' && dir_is_empty(e->target)) {
        if (account_copy_tree(e->seed, e->target) < 0) {
            fprintf(stderr, "fsinit: seeding %s from %s: %s\n", e->target, e->seed, strerror(errno));
            return -1;
        }
        printf("fsinit: seeded %s from %s\n", e->target, e->seed);
    }
    if (e->homes) {
        if (migrate(e->target) < 0) {
            fprintf(stderr, "fsinit: converting the layout of %s: %s\n", e->target, strerror(errno));
            return -1;
        }
        if (make_homes(e->target) < 0)
            return -1;
        if (add_wheel(e->target) < 0) {
            fprintf(stderr, "fsinit: adding the group wheel to %s: %s\n", e->target, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* Mounts every folder of the host at /mnt/TAG (docs/design/9p.md). The
 * kernel lists the mount tags of the virtio-9p devices in /dev/9p. A
 * mount point that is mounted already is skipped. A failure is reported.
 * It does not change the exit status, because no share is required for
 * the boot. */
static void mount_shares(void)
{
    DIR *d = opendir("/dev/9p");
    if (d == NULL)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char target[300];
        snprintf(target, sizeof target, "/mnt/%s", de->d_name);
        if (is_mounted(target)) {
            note("%s mounted already on %s", de->d_name, target);
            continue;
        }
        if ((mkdir("/mnt", 0755) < 0 && errno != EEXIST) || (mkdir(target, 0755) < 0 && errno != EEXIST)) {
            fprintf(stderr, "fsinit: %s: %s\n", target, strerror(errno));
            continue;
        }
        if (mount_options(de->d_name, target, "9p", NULL) < 0) {
            fprintf(stderr, "fsinit: mount the host folder %s on %s: %s\n", de->d_name, target, strerror(errno));
            continue;
        }
        printf("fsinit: mounted the host folder %s on %s\n", de->d_name, target);
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    const char *table = "/etc/fstab";
    int opt;
    const char *migrate_dir = NULL;
    while ((opt = getopt(argc, argv, "f:vm:")) != -1) {
        if (opt == 'f')
            table = optarg;
        else if (opt == 'v')
            verbose = 1;
        else if (opt == 'm')
            migrate_dir = optarg;
        else {
            fprintf(stderr, "usage: fsinit [-f fstab] [-v] | fsinit -m DIR\n");
            return 2;
        }
    }
    if (migrate_dir) {
        if (migrate(migrate_dir) < 0) {
            fprintf(stderr, "fsinit: converting the layout of %s: %s\n", migrate_dir, strerror(errno));
            return 1;
        }
        if (add_wheel(migrate_dir) < 0) {
            fprintf(stderr, "fsinit: adding the group wheel to %s: %s\n", migrate_dir, strerror(errno));
            return 1;
        }
        return 0;
    }
    FILE *f = fopen(table, "r");
    if (f == NULL) {
        fprintf(stderr, "fsinit: %s: %s\n", table, strerror(errno));
        mount_shares();
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
    mount_shares();
    return status;
}
