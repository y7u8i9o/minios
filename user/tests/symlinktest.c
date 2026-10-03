/* Symbolic link test, run by /etc/tests/symlink.sh (case symlink). The
 * runner attaches an empty mfs image as vdb and an empty FAT12 image as
 * vdc. The checks cover creation and readlink, relative and absolute
 * targets, chains and the limit of SYMLOOP_MAX links, loops, O_NOFOLLOW,
 * lstat against stat, dangling links and O_CREAT through them, unlink and
 * rename of links, ".." after a link, links across mount points, links on
 * the initrd and on the root image built by mkfs, the filesystems that
 * refuse links, and links that survive an unmount. The mfs volume remains
 * mounted on /mnt with its links; the script unmounts it for the post
 * script's fsck. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mount.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create %s: %s", path, strerror(errno));
    if (fd >= 0) {
        CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text), "write %s", path);
        close(fd);
    }
}

/* The contents of path, or "" when it cannot be read. */
static const char *read_file(const char *path)
{
    static char buf[128];
    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return buf;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    return buf;
}

/* The target of the link at path, or "" on failure. */
static const char *target_of(const char *path)
{
    static char buf[PATH_MAX];
    ssize_t n = readlink(path, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = '\0';
    return buf;
}

static int is_link(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static int fails_with(int result, int err)
{
    return result < 0 && errno == err;
}

static ino_t inode_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_ino : 0;
}

static int dirent_type(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    int type = -1;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, name) == 0)
            type = e->d_type;
    if (d)
        closedir(d);
    return type;
}

static void creation_and_readlink(void)
{
    CHECK(mkdir("/sl", 0755) == 0 && mkdir("/sl/dir", 0755) == 0 && mkdir("/sl/dir/sub", 0755) == 0,
          "mkdir /sl: %s", strerror(errno));
    write_file("/sl/dir/file.txt", "hello\n");
    CHECK(symlink("dir/file.txt", "/sl/rel") == 0, "symlink rel: %s", strerror(errno));
    char buf[32];
    memset(buf, 'X', sizeof buf);
    ssize_t n = readlink("/sl/rel", buf, sizeof buf);
    CHECK(n == 12 && memcmp(buf, "dir/file.txt", 12) == 0 && buf[12] == 'X',
          "readlink returns the byte count without a NUL (%zd)", n);
    memset(buf, 'X', sizeof buf);
    n = readlink("/sl/rel", buf, 4);
    CHECK(n == 4 && memcmp(buf, "dir/X", 5) == 0, "readlink truncates to the buffer (%zd)", n);
    CHECK(fails_with((int)readlink("/sl/dir/file.txt", buf, sizeof buf), EINVAL), "readlink of a file");
    CHECK(fails_with((int)readlink("/sl/none", buf, sizeof buf), ENOENT), "readlink of nothing");
    CHECK(fails_with(symlink("x", "/sl/rel"), EEXIST), "symlink over an existing name");
    CHECK(fails_with(symlink("", "/sl/empty"), ENOENT), "symlink with an empty target");
    CHECK(strcmp(read_file("/sl/rel"), "hello\n") == 0, "read through a relative link");
    CHECK(symlink("/sl/dir/file.txt", "/sl/abs") == 0, "symlink abs: %s", strerror(errno));
    CHECK(strcmp(read_file("/sl/abs"), "hello\n") == 0, "read through an absolute link");
    CHECK(dirent_type("/sl", "abs") == DT_LNK, "getdents reports DT_LNK");

    /* A relative target is resolved from the directory containing the link,
     * not from the working directory. */
    CHECK(symlink("file.txt", "/sl/dir/in") == 0, "symlink in dir");
    CHECK(chdir("/sl") == 0, "chdir /sl");
    CHECK(strcmp(read_file("dir/in"), "hello\n") == 0, "relative link from another directory");
    CHECK(chdir("/") == 0, "chdir /");

    /* The *at forms resolve from a directory descriptor. */
    int dfd = open("/sl", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0, "open /sl");
    CHECK(symlinkat("dir/file.txt", dfd, "at") == 0, "symlinkat: %s", strerror(errno));
    n = readlinkat(dfd, "at", buf, sizeof buf);
    CHECK(n == 12 && memcmp(buf, "dir/file.txt", 12) == 0, "readlinkat (%zd)", n);
    struct stat st;
    CHECK(fstatat(dfd, "at", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode), "fstatat nofollow");
    CHECK(fstatat(dfd, "at", &st, 0) == 0 && S_ISREG(st.st_mode), "fstatat follows");
    close(dfd);

    /* A target of PATH_MAX bytes and more is refused; the longest target
     * the kernel accepts is stored and read back whole. */
    char longtarget[300];
    memset(longtarget, 'a', sizeof longtarget);
    longtarget[256] = '\0';
    CHECK(fails_with(symlink(longtarget, "/sl/long"), ENAMETOOLONG), "symlink with a 256 byte target");
    longtarget[255] = '\0';
    CHECK(symlink(longtarget, "/sl/long") == 0, "symlink with a 255 byte target: %s", strerror(errno));
    CHECK(strcmp(target_of("/sl/long"), longtarget) == 0, "255 byte target read back");
}

static void chains_and_loops(void)
{
    CHECK(symlink("c2", "/sl/c1") == 0 && symlink("c3", "/sl/c2") == 0 && symlink("dir/file.txt", "/sl/c3") == 0,
          "chain");
    CHECK(strcmp(read_file("/sl/c1"), "hello\n") == 0, "read through a chain");
    CHECK(symlink("dir", "/sl/dl") == 0, "link to a directory");
    CHECK(strcmp(read_file("/sl/dl/file.txt"), "hello\n") == 0, "link as an intermediate component");
    CHECK(strcmp(read_file("/sl/dl/in"), "hello\n") == 0, "two links in one path");

    /* 41 names n0 .. n40, each a link to the next, n40 to the file: n1
     * needs 40 links and resolves, n0 needs 41 and fails. */
    char name[32], next[32];
    for (int i = 0; i <= 40; i++) {
        snprintf(name, sizeof name, "/sl/n%d", i);
        snprintf(next, sizeof next, "n%d", i + 1);
        CHECK(symlink(i == 40 ? "dir/file.txt" : next, name) == 0, "symlink %s", name);
    }
    CHECK(strcmp(read_file("/sl/n1"), "hello\n") == 0, "40 links are followed");
    CHECK(fails_with(open("/sl/n0", O_RDONLY), ELOOP), "41 links give ELOOP");

    CHECK(symlink("loop2", "/sl/loop1") == 0 && symlink("loop1", "/sl/loop2") == 0, "loop");
    CHECK(symlink("self", "/sl/self") == 0, "self loop");
    struct stat st;
    CHECK(fails_with(stat("/sl/loop1", &st), ELOOP), "stat of a loop: %s", strerror(errno));
    CHECK(fails_with(open("/sl/self", O_RDONLY), ELOOP), "open of a self loop");
    CHECK(fails_with(open("/sl/loop1/x", O_RDONLY), ELOOP), "loop in an intermediate component");
    CHECK(lstat("/sl/loop1", &st) == 0 && S_ISLNK(st.st_mode), "lstat of a loop");
    CHECK(fails_with(open("/sl/loop1", O_WRONLY | O_CREAT, 0644), ELOOP), "O_CREAT through a loop");
}

static void nofollow_and_lstat(void)
{
    CHECK(fails_with(open("/sl/abs", O_RDONLY | O_NOFOLLOW), ELOOP), "O_NOFOLLOW on a link");
    int fd = open("/sl/dl/file.txt", O_RDONLY | O_NOFOLLOW);
    CHECK(fd >= 0, "O_NOFOLLOW follows intermediate links: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
    CHECK(fails_with(open("/sl/abs", O_WRONLY | O_CREAT | O_NOFOLLOW, 0644), ELOOP), "O_CREAT with O_NOFOLLOW");

    struct stat ls, ss;
    CHECK(lstat("/sl/abs", &ls) == 0 && S_ISLNK(ls.st_mode) && ls.st_size == (off_t)strlen("/sl/dir/file.txt"),
          "lstat reports the link and its target length");
    CHECK(stat("/sl/abs", &ss) == 0 && S_ISREG(ss.st_mode) && ss.st_size == 6, "stat reports the target");
    CHECK(ls.st_ino != ss.st_ino, "link and target are different inodes");
    CHECK(lstat("/sl/dl/", &ls) == 0 && S_ISDIR(ls.st_mode), "a trailing slash follows the link");

    /* utimensat with AT_SYMLINK_NOFOLLOW sets the time of the link only. */
    struct timespec times[2] = { { 1000, 0 }, { 1000, 0 } };
    CHECK(utimensat(AT_FDCWD, "/sl/abs", times, AT_SYMLINK_NOFOLLOW) == 0, "utimensat nofollow: %s",
          strerror(errno));
    CHECK(lstat("/sl/abs", &ls) == 0 && ls.st_mtime == 1000, "link time set");
    CHECK(stat("/sl/abs", &ss) == 0 && ss.st_mtime != 1000, "target time retained");
}

static void dangling(void)
{
    struct stat st;
    CHECK(symlink("created.txt", "/sl/dang") == 0, "dangling link");
    CHECK(fails_with(stat("/sl/dang", &st), ENOENT), "stat of a dangling link");
    CHECK(lstat("/sl/dang", &st) == 0 && S_ISLNK(st.st_mode), "lstat of a dangling link");
    CHECK(fails_with(open("/sl/dang", O_RDONLY), ENOENT), "open of a dangling link");
    CHECK(fails_with(open("/sl/dang", O_WRONLY | O_CREAT | O_EXCL, 0644), EEXIST), "O_EXCL on a link");
    /* O_CREAT through a dangling link creates its target (POSIX). */
    int fd = open("/sl/dang", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "O_CREAT through a dangling link: %s", strerror(errno));
    if (fd >= 0) {
        CHECK(write(fd, "made\n", 5) == 5, "write through the link");
        close(fd);
    }
    CHECK(strcmp(read_file("/sl/created.txt"), "made\n") == 0, "the target was created");
    CHECK(is_link("/sl/dang"), "the link remains a link");
    CHECK(symlink("/sl/nodir/x", "/sl/dang2") == 0, "link into a missing directory");
    CHECK(fails_with(open("/sl/dang2", O_WRONLY | O_CREAT, 0644), ENOENT), "O_CREAT with a missing directory");
    CHECK(symlink("dir/newfile", "/sl/dang3") == 0, "second dangling link");
    CHECK(chdir("/sl/dir/sub") == 0, "chdir sub");
    fd = open("../../dang3", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "relative O_CREAT through a link: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
    CHECK(chdir("/") == 0 && stat("/sl/dir/newfile", &st) == 0, "created beside the link, not the cwd");
}

static void unlink_rename_and_dotdot(void)
{
    struct stat st;
    CHECK(unlink("/sl/rel") == 0, "unlink a link");
    CHECK(lstat("/sl/rel", &st) < 0 && stat("/sl/dir/file.txt", &st) == 0, "the target survives unlink");
    CHECK(symlink("dir", "/sl/dl2") == 0 && unlink("/sl/dl2") == 0, "unlink a link to a directory");
    CHECK(stat("/sl/dir/file.txt", &st) == 0, "the directory survives");
    CHECK(fails_with(rmdir("/sl/dl"), ENOTDIR), "rmdir of a link to a directory");

    CHECK(rename("/sl/abs", "/sl/abs2") == 0, "rename a link");
    CHECK(strcmp(target_of("/sl/abs2"), "/sl/dir/file.txt") == 0 && lstat("/sl/abs", &st) < 0,
          "the renamed link retains its target");
    /* A relative link moved to another directory resolves from there. */
    CHECK(rename("/sl/dir/in", "/sl/in") == 0, "move a relative link");
    CHECK(fails_with(stat("/sl/in", &st), ENOENT), "the moved relative link now dangles");
    CHECK(rename("/sl/in", "/sl/dir/in") == 0 && stat("/sl/dir/in", &st) == 0, "and resolves when moved back");
    write_file("/sl/victim", "victim\n");
    CHECK(rename("/sl/abs2", "/sl/victim") == 0 && is_link("/sl/victim"), "a link replaces a file");
    CHECK(strcmp(read_file("/sl/dir/file.txt"), "hello\n") == 0, "the target of the replacing link is intact");

    CHECK(link("/sl/victim", "/sl/hard") == 0, "hard link to a link: %s", strerror(errno));
    CHECK(lstat("/sl/hard", &st) == 0 && S_ISLNK(st.st_mode) && st.st_nlink == 2, "the hard link is the link");

    /* ".." after a link leaves the directory the link led to. */
    CHECK(symlink("/sl/dir/sub", "/sl/deep") == 0, "link to a nested directory");
    CHECK(inode_of("/sl/deep/..") == inode_of("/sl/dir"), "\"..\" after a link is physical");
    CHECK(chdir("/sl/deep") == 0, "chdir through a link");
    char cwd[PATH_MAX];
    CHECK(getcwd(cwd, sizeof cwd) && strcmp(cwd, "/sl/dir/sub") == 0, "getcwd without links: %s", cwd);
    CHECK(inode_of("..") == inode_of("/sl/dir"), "\"..\" of the working directory");
    CHECK(chdir("/") == 0, "chdir /");
    char real[PATH_MAX];
    CHECK(realpath("/sl/c1", real) && strcmp(real, "/sl/dir/file.txt") == 0, "realpath of a chain: %s", real);
    CHECK(realpath("/sl/deep/../in", real) && strcmp(real, "/sl/dir/file.txt") == 0, "realpath with \"..\": %s",
          real);
    CHECK(realpath("/sl/loop1", real) == NULL && errno == ELOOP, "realpath of a loop");
    CHECK(realpath("/sl/dang2", real) == NULL && errno == ENOENT, "realpath of a dangling link");
}

static void mounts(void)
{
    struct stat st;
    CHECK(mkdir("/mnt", 0755) == 0, "mkdir /mnt");
    CHECK(mount("vdb", "/mnt", "mfs") == 0, "mount vdb: %s", strerror(errno));
    write_file("/mnt/m.txt", "on mnt\n");
    CHECK(symlink("m.txt", "/mnt/in") == 0, "link inside the mount");
    CHECK(symlink("../sl/dir/file.txt", "/mnt/out") == 0, "link out of the mount");
    CHECK(symlink("/mnt/m.txt", "/sl/tomnt") == 0, "link into the mount");
    CHECK(symlink("/mnt", "/sl/mntdir") == 0, "link to the mount point");
    CHECK(strcmp(read_file("/mnt/in"), "on mnt\n") == 0, "read inside the mount");
    CHECK(strcmp(read_file("/mnt/out"), "hello\n") == 0, "\"..\" in a target leaves the mount");
    CHECK(strcmp(read_file("/sl/tomnt"), "on mnt\n") == 0, "read into the mount");
    CHECK(strcmp(read_file("/sl/mntdir/in"), "on mnt\n") == 0, "a link crosses the mount point");
    CHECK(inode_of("/sl/mntdir/..") == inode_of("/"), "\"..\" of the mounted root through a link");
    CHECK(chdir("/sl/mntdir") == 0, "chdir into the mount through a link");
    char cwd[PATH_MAX];
    CHECK(getcwd(cwd, sizeof cwd) && strcmp(cwd, "/mnt") == 0, "getcwd is the mount point: %s", cwd);
    CHECK(chdir("/") == 0, "chdir /");

    /* The links survive an unmount; umount through a link names /mnt. */
    sync();
    CHECK(umount("/sl/mntdir") == 0, "umount through a link: %s", strerror(errno));
    CHECK(lstat("/mnt/in", &st) < 0, "unmounted");
    CHECK(mount("vdb", "/mnt", "mfs") == 0, "mount again: %s", strerror(errno));
    CHECK(strcmp(target_of("/mnt/in"), "m.txt") == 0, "link target after remount");
    CHECK(strcmp(read_file("/mnt/out"), "hello\n") == 0, "link resolves after remount");

    /* The initrd contains links from the tar archive, the root image the
     * same links from mkfs. */
    CHECK(is_link("/initrd/etc/tests/link-fixture.tar"), "link on the initrd");
    CHECK(strcmp(target_of("/initrd/etc/tests/link-fixture.tar"), "fixture.tar") == 0, "initrd link target");
    struct stat a, b;
    CHECK(stat("/initrd/etc/tests/link-fixture.tar", &a) == 0 && stat("/initrd/etc/tests/fixture.tar", &b) == 0 &&
          a.st_ino == b.st_ino && a.st_dev == b.st_dev, "relative initrd link remains on the initrd");
    CHECK(strcmp(target_of("/initrd/etc/tests/link-motd"), "/etc/motd") == 0, "absolute initrd link target");
    CHECK(stat("/initrd/etc/tests/link-motd", &a) == 0 && stat("/etc/motd", &b) == 0 && a.st_ino == b.st_ino &&
          a.st_dev == b.st_dev, "absolute initrd link resolves from the root");
    CHECK(fails_with(symlink("x", "/initrd/new"), EROFS), "symlink on the initrd");
    CHECK(strcmp(target_of("/etc/tests/link-fixture.tar"), "fixture.tar") == 0, "mkfs stored the link");
    CHECK(stat("/etc/tests/link-fixture.tar", &a) == 0 && S_ISREG(a.st_mode), "mkfs link resolves");

    CHECK(fails_with(symlink("x", "/dev/newlink"), EPERM), "symlink on devfs");
    CHECK(mkdir("/fat", 0755) == 0 && mount("vdc", "/fat", "fat") == 0, "mount fat: %s", strerror(errno));
    CHECK(fails_with(symlink("x", "/fat/l"), EPERM), "symlink on FAT");
    CHECK(symlink("/fat", "/sl/fatdir") == 0, "link to the FAT mount");
    write_file("/sl/fatdir/through.txt", "fat\n");
    CHECK(strcmp(read_file("/fat/through.txt"), "fat\n") == 0, "FAT lookups through a link");
    CHECK(unlink("/fat/through.txt") == 0, "unlink on FAT");
    CHECK(umount("/fat") == 0, "umount fat: %s", strerror(errno));
}

int main(void)
{
    creation_and_readlink();
    chains_and_loops();
    nofollow_and_lstat();
    dangling();
    unlink_rename_and_dotdot();
    mounts();
    printf("symlinktest: %d failures\n", failures);
    return failures ? 1 : 0;
}
