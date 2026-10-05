/* The back end of the installer (docs/design/installer.md). It writes a
 * GPT with an EFI system partition, a swap partition and a root partition
 * to the target disk with part, formats the partitions with mkfat and
 * mkfs, mounts them on INST_TARGET and INST_TARGET/boot, installs the
 * chosen package group from the signed repository of the medium with
 * pkg --root, and then configures the installed system: the kernel
 * command line, /etc/fstab, the accounts, the language, the keyboard
 * layout, the time zone and, on x86_64, the BIOS stage of the boot loader.
 * The programs it runs are the ones an administrator would run, and their
 * output goes to the console. */
#include "installer.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <minios/account.h>
#include <minios/disk.h>

#define TYPE_ESP   "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
#define TYPE_SWAP  "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"
#define TYPE_BIOS  "21686148-6449-6e6f-744e-656564454649"
#define TYPE_REPO  "6d696e69-6f73-4e70-6b67-7265706f7369"

static const char *machine(void)
{
    static struct utsname u;
    if (!u.machine[0] && uname(&u) < 0)
        strcpy(u.machine, "x86_64");
    return u.machine;
}

static const char *type_root(void)
{
    return strcmp(machine(), "aarch64") == 0 ? "b921b045-1df0-41c3-af44-4c6f280d3fae"
                                              : "4f68bce3-e8cd-4db1-96e7-fbcaf984b709";
}

void inst_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("installer: ");
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
    FILE *f = fopen(INST_LOG, "a");
    if (f) {
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        va_end(ap);
        fclose(f);
    }
}

/* Run a program to its end. Returns 0 when it exits with status 0. */
static int run(const char *const argv[])
{
    char line[512] = "";
    for (int i = 0; argv[i]; i++) {
        strlcat(line, argv[i], sizeof line);
        strlcat(line, " ", sizeof line);
    }
    inst_log("running %s", line);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    inst_log("%s failed", argv[0]);
    return -1;
}

/* A partition of disk from /dev/partitions ("name disk partuuid typeuuid
 * bytes") with the type GUID type, the first if several. */
static int find_partition(const char *disk, const char *type, char *name, size_t nsize, char *uuid, size_t usize)
{
    FILE *f = fopen("/dev/partitions", "r");
    char line[256], n[32], d[32], u[64], t[64];
    int found = 0;
    while (f && !found && fgets(line, sizeof line, f))
        if (sscanf(line, "%31s %31s %63s %63s", n, d, u, t) == 4 && strcmp(d, disk) == 0 && strcmp(t, type) == 0) {
            snprintf(name, nsize, "%s", n);
            snprintf(uuid, usize, "%s", u);
            found = 1;
        }
    if (f)
        fclose(f);
    return found ? 0 : -1;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f || fputs(text, f) < 0 || fclose(f) != 0) {
        inst_log("%s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int mkdirs(const char *path)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    return mkdir(buf, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

/* The directory of the medium: INST_MEDIUM, where the repo partition of an
 * installation medium is mounted, or / on the live medium. */
static char medium_dir[64] = INST_MEDIUM;

const char *inst_medium_dir(void)
{
    return medium_dir;
}

const char *inst_answers_path(void)
{
    static char path[96];
    snprintf(path, sizeof path, "%s/" INST_ANSWERS, strcmp(medium_dir, "/") == 0 ? "" : medium_dir);
    return path;
}

int inst_is_disk_name(const char *name)
{
    size_t n = strlen(name), i;
    if ((strncmp(name, "vd", 2) == 0 || strncmp(name, "sd", 2) == 0) && n > 2) {
        for (i = 2; i < n && name[i] >= 'a' && name[i] <= 'z'; i++)
            ;
        return i == n;
    }
    if (strncmp(name, "nvme", 4) != 0)
        return 0;
    i = 4;
    size_t start = i;
    while (i < n && isdigit((unsigned char)name[i]))
        i++;
    if (i == start || i >= n || name[i++] != 'n')
        return 0;
    start = i;
    while (i < n && isdigit((unsigned char)name[i]))
        i++;
    return i > start && i == n;
}

/* The volume identifier of an ISO 9660 image at the start of the device
 * path, without its trailing blanks, or "" when the device has none. */
static void iso_volume_id(const char *path, char *id, size_t size)
{
    unsigned char vd[2048];
    id[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = lseek(fd, 16 * 2048, SEEK_SET) == 16 * 2048 ? read(fd, vd, sizeof vd) : -1;
    close(fd);
    if (n != (ssize_t)sizeof vd || vd[0] != 1 || memcmp(vd + 1, "CD001", 5) != 0)
        return;
    size_t len = 32;
    while (len && (vd[40 + len - 1] == ' ' || vd[40 + len - 1] == '\0'))
        len--;
    snprintf(id, size, "%.*s", (int)len, (const char *)vd + 40);
}

/* The live medium (docs/design/live.md): the root file system is the
 * medium, and /etc/live-medium names the volume identifier of its image.
 * The medium has no disk when it is in a CD drive. When the image was
 * written to a disk, such as a USB stick, that disk is the medium. */
static int find_live_medium(char *disk, size_t size)
{
    char index[256], volid[64] = "";
    snprintf(index, sizeof index, "/repo/%s/index", machine());
    FILE *f = fopen("/etc/live-medium", "r");
    if (!f)
        return -1;
    if (!fgets(volid, sizeof volid, f))
        volid[0] = '\0';
    fclose(f);
    volid[strcspn(volid, "\n")] = '\0';
    if (!volid[0] || access(index, R_OK) != 0)
        return -1;
    snprintf(medium_dir, sizeof medium_dir, "/");
    disk[0] = '\0';
    DIR *d = opendir("/dev");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!inst_is_disk_name(e->d_name))
            continue;
        char path[32], id[40];
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        iso_volume_id(path, id, sizeof id);
        if (strcmp(id, volid) == 0) {
            snprintf(disk, size, "%s", e->d_name);
            break;
        }
    }
    if (d)
        closedir(d);
    inst_log("the live medium %s%s%s is the medium", volid, disk[0] ? " on " : "", disk);
    return 0;
}

int inst_find_medium(char *disk, size_t size)
{
    if (find_live_medium(disk, size) == 0)
        return 0;
    FILE *f = fopen("/dev/partitions", "r");
    char line[256], n[32], d[32], u[64], t[64], index[256], mounted[256];
    int found = 0;
    mkdirs(INST_MEDIUM);
    /* A medium that an earlier run mounted is used again, with the name of
     * its disk that the run wrote down. */
    snprintf(index, sizeof index, INST_MEDIUM "/repo/%s/index", machine());
    FILE *m = fopen(INST_DIR "/medium.disk", "r");
    if (m && access(index, R_OK) == 0 && fgets(mounted, sizeof mounted, m)) {
        mounted[strcspn(mounted, "\n")] = '\0';
        snprintf(disk, size, "%s", mounted);
        found = 1;
    }
    if (m)
        fclose(m);
    while (f && !found && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s %31s %63s %63s", n, d, u, t) != 4 || strcmp(t, TYPE_REPO) != 0)
            continue;
        if (mount(n, INST_MEDIUM, "mfs") < 0)
            continue;
        snprintf(index, sizeof index, INST_MEDIUM "/repo/%s/index", machine());
        if (access(index, R_OK) == 0) {
            snprintf(disk, size, "%s", d);
            snprintf(mounted, sizeof mounted, "%s\n", d);
            write_text(INST_DIR "/medium.disk", mounted);
            found = 1;
        } else {
            umount(INST_MEDIUM);
        }
    }
    if (f)
        fclose(f);
    return found ? 0 : -1;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

int inst_read_answers(const char *path, struct plan *p)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[600];
    p->poweroff = 1;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *v = strchr(line, ' ');
        if (line[0] == '#' || !v)
            continue;
        *v++ = '\0';
        while (*v == ' ')
            v++;
        static const struct { const char *key; size_t off, size; } keys[] = {
            { "disk", offsetof(struct plan, disk), sizeof ((struct plan *)0)->disk },
            { "group", offsetof(struct plan, group), sizeof ((struct plan *)0)->group },
            { "packages", offsetof(struct plan, packages), sizeof ((struct plan *)0)->packages },
            { "lang", offsetof(struct plan, lang), sizeof ((struct plan *)0)->lang },
            { "keymap", offsetof(struct plan, keymap), sizeof ((struct plan *)0)->keymap },
            { "timezone", offsetof(struct plan, timezone), sizeof ((struct plan *)0)->timezone },
            { "root_password", offsetof(struct plan, root_password), sizeof ((struct plan *)0)->root_password },
            { "user", offsetof(struct plan, user), sizeof ((struct plan *)0)->user },
            { "user_fullname", offsetof(struct plan, user_fullname), sizeof ((struct plan *)0)->user_fullname },
            { "user_password", offsetof(struct plan, user_password), sizeof ((struct plan *)0)->user_password },
            { "reuse_home", offsetof(struct plan, reuse_home), sizeof ((struct plan *)0)->reuse_home },
            { "cmdline", offsetof(struct plan, cmdline), sizeof ((struct plan *)0)->cmdline },
        };
        int known = 0;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (strcmp(line, keys[i].key) == 0) {
                snprintf((char *)p + keys[i].off, keys[i].size, "%s", v);
                known = 1;
            }
        if (strcmp(line, "swap_mb") == 0) {
            p->swap_mb = atoi(v);
            known = 1;
        } else if (strcmp(line, "poweroff") == 0) {
            p->poweroff = strcmp(v, "no") != 0;
            known = 1;
        }
        if (!known)
            inst_log("%s: unknown key %s, ignored", path, line);
    }
    fclose(f);
    return 0;
}

void inst_defaults(struct plan *p)
{
    if (!p->group[0])
        strcpy(p->group, "desktop-system");
    if (!p->lang[0])
        strcpy(p->lang, "en_US.UTF-8");
    if (!p->keymap[0])
        strcpy(p->keymap, "us");
    if (!p->timezone[0])
        strcpy(p->timezone, "UTC");
    if (p->swap_mb <= 0)
        p->swap_mb = 256;
}

int inst_list_disks(const char *medium_disk, char (*names)[16], long *mib, int max)
{
    DIR *d = opendir("/dev");
    struct dirent *e;
    int n = 0;
    while (d && n < max && (e = readdir(d))) {
        /* Disks only: no partitions and no CD drives. */
        if (!inst_is_disk_name(e->d_name) || strcmp(e->d_name, medium_disk) == 0)
            continue;
        char path[32];
        struct stat st;
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode) || st.st_size == 0)
            continue;
        snprintf(names[n], 16, "%s", e->d_name);
        mib[n] = (long)(st.st_size >> 20);
        n++;
    }
    if (d)
        closedir(d);
    return n;
}

int inst_check(const struct plan *p, const char *medium_disk)
{
    char path[32];
    struct stat st;
    snprintf(path, sizeof path, "/dev/%s", p->disk);
    if (!p->disk[0] || !inst_is_disk_name(p->disk) || stat(path, &st) < 0 || !S_ISBLK(st.st_mode)) {
        inst_log("the target disk %s does not exist", p->disk[0] ? p->disk : "(none)");
        return -1;
    }
    if (medium_disk[0] && strcmp(p->disk, medium_disk) == 0) {
        inst_log("%s is the installation medium", p->disk);
        return -1;
    }
    /* The EFI system partition, swap and a root of at least 512 MiB. */
    long need = 256 + p->swap_mb + 512 + 4;
    if ((st.st_size >> 20) < need) {
        inst_log("%s has %ld MiB, and the installation needs %ld", p->disk, (long)(st.st_size >> 20), need);
        return -1;
    }
    if (!p->root_password[0]) {
        inst_log("the password of root is missing");
        return -1;
    }
    /* The installer environment contains the packages keymaps and tzdata,
     * which the target receives as well. */
    char keymap[64];
    snprintf(keymap, sizeof keymap, "/usr/share/keymaps/%.32s.mkm", p->keymap);
    if (stat(keymap, &st) < 0) {
        inst_log("there is no keyboard layout %s in /usr/share/keymaps", p->keymap);
        return -1;
    }
    char zone[128];
    snprintf(zone, sizeof zone, "/usr/share/zoneinfo/%s", p->timezone);
    if (strcmp(p->timezone, "UTC") != 0 && stat(zone, &st) < 0) {
        inst_log("there is no time zone %s in /usr/share/zoneinfo", p->timezone);
        return -1;
    }
    if (p->user[0] && (!account_name_valid(p->user) || !p->user_password[0])) {
        inst_log("the account %s needs a valid name and a password", p->user);
        return -1;
    }
    return 0;
}

/* Replace or append the line "key=value" of a desktop.conf. */
static int conf_line(const char *path, const char *key, const char *value)
{
    char line[256];
    snprintf(line, sizeof line, "%s=%s", key, value);
    /* account_replace matches the first field separated by ":", which a
     * key=value file lacks, and the file is therefore rewritten here. */
    FILE *in = fopen(path, "r");
    char tmp[256], buf[512];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        if (in)
            fclose(in);
        return -1;
    }
    int done = 0;
    size_t klen = strlen(key);
    while (in && fgets(buf, sizeof buf, in)) {
        if (strncmp(buf, key, klen) == 0 && buf[klen] == '=') {
            fprintf(out, "%s\n", line);
            done = 1;
        } else {
            fputs(buf, out);
        }
    }
    if (!done)
        fprintf(out, "%s\n", line);
    if (in)
        fclose(in);
    if (fclose(out) != 0 || rename(tmp, path) < 0)
        return -1;
    return 0;
}

/* The fstab of the target: the entry of the data volume of the image is
 * dropped, or names the reused volume, and /boot is the EFI system
 * partition. */
static int write_fstab(const struct plan *p, const char *esp_uuid)
{
    char path[128], tmp[128], buf[512];
    snprintf(path, sizeof path, INST_TARGET "/etc/fstab");
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!in || !out) {
        if (in)
            fclose(in);
        if (out)
            fclose(out);
        inst_log("%s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(buf, sizeof buf, in)) {
        char dev[64], dir[64];
        if (sscanf(buf, "%63s %63s", dev, dir) == 2 && dev[0] != '#' && strcmp(dir, "/home") == 0)
            continue;
        fputs(buf, out);
    }
    fprintf(out, "# The EFI system partition, which contains the kernel and the boot loader.\n");
    fprintf(out, "PARTUUID=%s /boot fat\n", esp_uuid);
    if (p->reuse_home[0])
        fprintf(out, "# The data volume of an earlier installation.\n%s /home mfs homes\n", p->reuse_home);
    fclose(in);
    if (fclose(out) != 0 || rename(tmp, path) < 0) {
        inst_log("%s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* The account databases of the target, /etc/passwd and the others, are
 * symbolic links to /usr/local/etc, which leads to /home/.local/etc
 * (docs/design/users.md). The homes are seeded as fsinit seeds a fresh
 * data volume, and the accounts are written there. */
static int accounts(const struct plan *p)
{
    char passwd[160], group[160], shadow[160], line[512], hash[128], home[160];
    snprintf(passwd, sizeof passwd, INST_TARGET "/home/.local/etc/passwd");
    snprintf(group, sizeof group, INST_TARGET "/home/.local/etc/group");
    snprintf(shadow, sizeof shadow, INST_TARGET "/home/.local/etc/shadow");
    if (account_copy_tree(INST_TARGET "/usr/share/skel/home", INST_TARGET "/home") < 0 ||
        account_copy_tree(INST_TARGET "/etc/skel", INST_TARGET "/root") < 0) {
        inst_log("seeding the homes: %s", strerror(errno));
        return -1;
    }
    if (account_hash(p->root_password, hash, sizeof hash) < 0)
        return -1;
    snprintf(line, sizeof line, "root:%s:%ld::::::", hash, account_today());
    if (account_replace(shadow, "root", line) < 0)
        return -1;
    inst_log("set the password of root");
    if (!p->user[0])
        return 0;
    /* The seed contains the account user, uid 1000, which the first account
     * replaces. */
    if (strcmp(p->user, "user") != 0) {
        account_replace(passwd, "user", NULL);
        account_replace(shadow, "user", NULL);
        account_replace(group, "user", NULL);
    }
    snprintf(line, sizeof line, "%s:x:%d:%d:%s:/home/%s:/bin/sh", p->user, ACCOUNT_FIRST_ID, ACCOUNT_FIRST_ID,
             p->user_fullname[0] ? p->user_fullname : p->user, p->user);
    if (account_replace(passwd, p->user, line) < 0)
        return -1;
    snprintf(line, sizeof line, "%s:x:%d:", p->user, ACCOUNT_FIRST_ID);
    if (account_replace(group, p->user, line) < 0)
        return -1;
    snprintf(line, sizeof line, "wheel:x:10:%s", p->user);
    if (account_replace(group, "wheel", line) < 0 || account_hash(p->user_password, hash, sizeof hash) < 0)
        return -1;
    snprintf(line, sizeof line, "%s:%s:%ld::::::", p->user, hash, account_today());
    if (account_replace(shadow, p->user, line) < 0)
        return -1;
    snprintf(home, sizeof home, INST_TARGET "/home/%s", p->user);
    if ((mkdir(home, 0700) < 0 && errno != EEXIST) || account_copy_tree(INST_TARGET "/etc/skel", home) < 0 ||
        account_chown_tree(home, ACCOUNT_FIRST_ID, ACCOUNT_FIRST_ID) < 0) {
        inst_log("%s: %s", home, strerror(errno));
        return -1;
    }
    inst_log("made the account %s, a member of wheel", p->user);
    return 0;
}

int inst_install(const struct plan *p)
{
    char dev[32], esp[16], root[16], swap[16], bios[16], esp_uuid[64], root_uuid[64], swap_uuid[64];
    char bios_uuid[64];
    char swapspec[32], rootspec[48], line[512];
    bool x86 = strcmp(machine(), "x86_64") == 0;
    snprintf(dev, sizeof dev, "/dev/%s", p->disk);
    snprintf(swapspec, sizeof swapspec, "swap:%d", p->swap_mb);
    snprintf(rootspec, sizeof rootspec, "root-%s:rest", machine());
    mkdirs(INST_EMPTY);

    inst_log("partitioning %s", p->disk);
    const char *part_bios[] = { "/usr/bin/part", dev, "0", "bios:1", "esp:256", swapspec, rootspec, NULL };
    const char *part_uefi[] = { "/usr/bin/part", dev, "0", "esp:256", swapspec, rootspec, NULL };
    if (run(x86 ? part_bios : part_uefi) < 0)
        return -1;
    if (find_partition(p->disk, TYPE_ESP, esp, sizeof esp, esp_uuid, sizeof esp_uuid) < 0 ||
        find_partition(p->disk, type_root(), root, sizeof root, root_uuid, sizeof root_uuid) < 0 ||
        find_partition(p->disk, TYPE_SWAP, swap, sizeof swap, swap_uuid, sizeof swap_uuid) < 0 ||
        (x86 && find_partition(p->disk, TYPE_BIOS, bios, sizeof bios, bios_uuid, sizeof bios_uuid) < 0)) {
        inst_log("the new partitions of %s are not in /dev/partitions", p->disk);
        return -1;
    }

    inst_log("formatting %s and %s", esp, root);
    char espdev[32], rootdev[32];
    snprintf(espdev, sizeof espdev, "/dev/%s", esp);
    snprintf(rootdev, sizeof rootdev, "/dev/%s", root);
    const char *mkfat[] = { "/usr/bin/mkfat", "-t", "32", espdev, "0", INST_EMPTY, NULL };
    const char *mkfs[] = { "/usr/bin/mkfs", rootdev, "0", INST_EMPTY, NULL };
    if (run(mkfat) < 0 || run(mkfs) < 0)
        return -1;
    mkdirs(INST_TARGET);
    if (mount(root, INST_TARGET, "mfs") < 0 || mkdirs(INST_TARGET "/boot") < 0 ||
        mount(esp, INST_TARGET "/boot", "fat") < 0) {
        inst_log("mounting the new file systems: %s", strerror(errno));
        return -1;
    }

    /* The command line exists before the kernel package, whose
     * installation writes the boot loader configuration from it. */
    mkdirs(INST_TARGET "/etc/kernel");
    snprintf(line, sizeof line, "root=PARTUUID=%s%s%s\n", root_uuid, p->cmdline[0] ? " " : "", p->cmdline);
    if (write_text(INST_TARGET "/etc/kernel/cmdline", line) < 0)
        return -1;
    if (x86) {
        snprintf(line, sizeof line, "PARTUUID=%s\n", bios_uuid);
        if (write_text(INST_TARGET "/etc/kernel/bios-disk", line) < 0)
            return -1;
    }

    inst_log("installing %s %s", p->group, p->packages);
    snprintf(line, sizeof line, "repo medium file://%s/repo/$arch\n",
             strcmp(medium_dir, "/") == 0 ? "" : medium_dir);
    if (write_text(INST_DIR "/pkg.conf", line) < 0)
        return -1;
    const char *update[] = { "/usr/bin/pkg", "--root", INST_TARGET, "--config", INST_DIR "/pkg.conf", "--keys",
                             "/etc/pkg/keys", "update", NULL };
    if (run(update) < 0)
        return -1;
    const char *install[64] = { "/usr/bin/pkg", "--root", INST_TARGET, "--config", INST_DIR "/pkg.conf", "--keys",
                                "/etc/pkg/keys", "install", p->group };
    int n = 9;
    char packages[sizeof p->packages];
    snprintf(packages, sizeof packages, "%s", p->packages);
    for (char *t = strtok(packages, " "); t && n < 63; t = strtok(NULL, " "))
        install[n++] = t;
    install[n] = NULL;
    if (run(install) < 0)
        return -1;
    /* The repository of the medium is the one the installed system knows
     * until its administrator configures another. */

    inst_log("configuring the installed system");
    if (write_fstab(p, esp_uuid) < 0)
        return -1;
    if (!p->reuse_home[0] && accounts(p) < 0) {
        inst_log("making the accounts failed");
        return -1;
    }
    if (conf_line(INST_TARGET "/etc/desktop.conf", "lang", p->lang) < 0 ||
        conf_line(INST_TARGET "/etc/desktop.conf", "keymap", p->keymap) < 0)
        inst_log("cannot set the language and the keyboard layout");
    if (strcmp(p->timezone, "UTC") != 0) {
        char zone[128];
        snprintf(zone, sizeof zone, "/usr/share/zoneinfo/%s", p->timezone);
        unlink(INST_TARGET "/etc/localtime");
        if (symlink(zone, INST_TARGET "/etc/localtime") < 0)
            inst_log("/etc/localtime: %s", strerror(errno));
    }
    const char *bootconfig[] = { "/usr/bin/pkg", "--root", INST_TARGET, "bootconfig", NULL };
    if (run(bootconfig) < 0)
        return -1;
    if (x86) {
        char index[8];
        const char *n = disk_partition_index(bios, p->disk);
        snprintf(index, sizeof index, "%s", n ? n : "");
        const char *limine[] = { "/usr/bin/limine", "bios-install", dev, index, NULL };
        if (run(limine) < 0)
            return -1;
    }

    mkdirs(INST_TARGET "/var/log");
    FILE *log = fopen(INST_LOG, "r"), *copy = fopen(INST_TARGET "/var/log/installer.log", "w");
    while (log && copy && fgets(line, sizeof line, log))
        fputs(line, copy);
    if (log)
        fclose(log);
    if (copy)
        fclose(copy);
    sync();
    if (umount(INST_TARGET "/boot") < 0 || umount(INST_TARGET) < 0) {
        inst_log("unmounting the target: %s", strerror(errno));
        return -1;
    }
    inst_log("the system is installed on %s", p->disk);
    return 0;
}
