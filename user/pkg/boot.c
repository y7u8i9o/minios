/* The boot loader configuration (docs/design/packages.md, P6 of
 * docs/plan/packaging.md). The package whose manifest names its kernel
 * file with a kernel line is the kernel of the system. pkg writes
 * <root>/boot/limine.conf from it and from /etc/kernel/cmdline whenever
 * that package changes and on pkg bootconfig, with an entry for the
 * previous kernel, which an upgrade saves as the kernel file with the
 * suffix .old. On an installed system /boot is the EFI system partition,
 * where Limine finds the file. The package whose manifest has a
 * bios-stage line is the boot loader: when it changes on the running
 * system and /etc/kernel/bios-disk names a disk, pkg runs limine
 * bios-install for that disk again. */
#include "pkg.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* The first line of a file of the root, without its newline, or "". */
static void first_line(const char *rel, char *out, size_t size)
{
    char path[PKG_PATH_MAX];
    root_path(path, sizeof path, rel);
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(out, (int)size, f))
        out[strcspn(out, "\n")] = '\0';
    fclose(f);
}

/* The path Limine reads a file of the root by. /boot is the partition
 * Limine loads its configuration from, boot(). */
static void boot_path(const char *rel, char *out, size_t size)
{
    if (strncmp(rel, "boot/", 5) == 0)
        snprintf(out, size, "boot():/%s", rel + 5);
    else
        snprintf(out, size, "boot():/%s", rel);
}

static void write_entry(FILE *f, const char *title, const char *path, const char *cmdline)
{
    fprintf(f, "/%s\n    protocol: limine\n    path: %s\n    cmdline: %s\n", title, path, cmdline);
    /* A video= option also selects the mode of the boot framebuffer, as
     * tools/mkiso.sh does for the CD. */
    const char *video = strstr(cmdline, "video=");
    if (video && (video == cmdline || video[-1] == ' ')) {
        char mode[32];
        size_t n = strcspn(video + 6, " @");
        if (n && n < sizeof mode) {
            memcpy(mode, video + 6, n);
            mode[n] = '\0';
            fprintf(f, "    resolution: %s\n", mode);
        }
    }
}

int boot_write_config(void)
{
    char (*names)[PKG_NAME_MAX];
    int count = db_names(&names);
    struct manifest m, kernel = { 0 };
    for (int i = 0; i < count; i++)
        if (db_read(names[i], &m) == 0 && m.kernel[0])
            kernel = m;
    free(names);
    if (!kernel.kernel[0])
        return 0;
    char cmdline[512], path[PKG_PATH_MAX], conf[PKG_PATH_MAX], tmp[PKG_PATH_MAX], old[PKG_PATH_MAX];
    first_line("etc/kernel/cmdline", cmdline, sizeof cmdline);
    root_path(conf, sizeof conf, "boot/limine.conf");
    snprintf(tmp, sizeof tmp, "%s.pkgtmp", conf);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -errno;
    fprintf(f, "# Written by pkg(1) from /etc/kernel/cmdline whenever the package of the\n"
               "# kernel changes, and by pkg bootconfig. A change made here is lost then.\n"
               "timeout: 3\nserial: yes\n\n");
    char title[128];
    snprintf(title, sizeof title, "minios %s", kernel.version);
    boot_path(kernel.kernel, path, sizeof path);
    write_entry(f, title, path, cmdline);
    struct stat st;
    snprintf(old, sizeof old, "%s.old", kernel.kernel);
    char oldfull[PKG_PATH_MAX];
    root_path(oldfull, sizeof oldfull, old);
    if (stat(oldfull, &st) == 0) {
        boot_path(old, path, sizeof path);
        write_entry(f, "minios, the previous kernel", path, cmdline);
    }
    int r = fflush(f) == 0 && ferror(f) == 0 ? 0 : -EIO;
    fclose(f);
    if (r == 0 && rename(tmp, conf) < 0)
        r = -errno;
    if (r < 0)
        unlink(tmp);
    return r;
}

/* Before a new kernel replaces target, the installed one becomes the
 * previous kernel. */
void boot_save_previous(const char *target)
{
    char old[PKG_PATH_MAX];
    struct stat st;
    snprintf(old, sizeof old, "%s.old", target);
    if (stat(target, &st) == 0 && rename(target, old) < 0)
        fprintf(stderr, "pkg: %s: cannot save the previous kernel: %s\n", target, strerror(errno));
}

/* The disk and the index of the partition with the unique GUID guid, from
 * /dev/partitions ("name disk partuuid typeuuid bytes"). */
static int partition_of(const char *guid, char *disk, size_t dsize, char *index, size_t isize)
{
    FILE *f = fopen("/dev/partitions", "r");
    char line[256], name[32], d[32], uuid[64];
    int found = 0;
    while (f && !found && fgets(line, sizeof line, f))
        if (sscanf(line, "%31s %31s %63s", name, d, uuid) == 3 && strcasecmp(uuid, guid) == 0 &&
            strncmp(name, d, strlen(d)) == 0) {
            snprintf(disk, dsize, "%s", d);
            snprintf(index, isize, "%s", name + strlen(d));
            found = 1;
        }
    if (f)
        fclose(f);
    return found ? 0 : -1;
}

/* /etc/kernel/bios-disk names the disk the BIOS boots from and, after a
 * space, the GPT partition index of its BIOS boot partition, or
 * PARTUUID=GUID of that partition, which the installer writes, since the
 * name of a disk depends on the disks attached. The installation into
 * another root leaves this to the installer. */
int boot_bios_install(void)
{
    char line[96], dev[48], index[16] = "";
    if (root[0])
        return 0;
    first_line("etc/kernel/bios-disk", line, sizeof line);
    if (strncmp(line, "PARTUUID=", 9) == 0) {
        if (partition_of(line + 9, dev + 5, sizeof dev - 5, index, sizeof index) < 0) {
            fprintf(stderr, "pkg: /etc/kernel/bios-disk: no partition %s\n", line + 9);
            return -1;
        }
    } else if (!line[0] || sscanf(line, "%31s %15s", dev + 5, index) < 1) {
        return 0;
    }
    memcpy(dev, "/dev/", 5);
    pid_t pid = fork();
    if (pid < 0)
        return -errno;
    if (pid == 0) {
        if (index[0])
            execl("/usr/bin/limine", "limine", "bios-install", dev, index, (char *)NULL);
        else
            execl("/usr/bin/limine", "limine", "bios-install", dev, (char *)NULL);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "pkg: limine bios-install %s failed\n", dev);
        return -EIO;
    }
    printf("installed the BIOS stage of the boot loader on %s\n", dev);
    return 0;
}
