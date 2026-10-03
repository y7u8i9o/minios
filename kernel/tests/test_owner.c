#include <tests/ktest.h>
#include <fs/vfs.h>
#include <sched/proc.h>
#include <sched/thread.h>
#include <lib/string.h>
#include <errno.h>

/* U1: ownership and modes. Files created on the mfs volume vdb as root
 * and as uid 1000 take the creator's ids and the requested mode less the
 * umask, chmod and chown follow the ownership rules, and everything
 * survives an unmount and a new mount. The FAT volume vdc reports the
 * owner and mask of its mount options, and devfs nodes change in memory. */

/* Give the test thread's process the given identity. The test runs in the
 * kernel process, which every later process of the boot would inherit, so
 * become_root restores the original state. */
static void become(uint32_t uid, uint32_t gid, uint32_t umask, int ngroups, const uint32_t *groups)
{
    struct proc *p = thread_current()->proc;
    spin_lock(&p->lock);
    p->cred.ruid = p->cred.euid = p->cred.suid = uid;
    p->cred.rgid = p->cred.egid = p->cred.sgid = gid;
    p->cred.umask = umask;
    p->cred.ngroups = ngroups;
    for (int i = 0; i < ngroups; i++)
        p->cred.groups[i] = groups[i];
    spin_unlock(&p->lock);
}

static void become_root(void)
{
    become(0, 0, 022, 0, NULL);
}

static void stat_of(const char *path, unsigned flags, struct stat *st)
{
    struct inode *ino;
    ktest_assert(vfs_lookup_path(path, flags, &ino, NULL, 0) == 0, "lookup %s", path);
    inode_stat(ino, st);
    inode_put(ino);
}

static void expect(const char *path, unsigned flags, uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct stat st;
    stat_of(path, flags, &st);
    ktest_assert((st.st_mode & 07777) == mode && st.st_uid == uid && st.st_gid == gid,
                 "%s: mode %o uid %u gid %u, want %o %u %u", path, st.st_mode & 07777, st.st_uid, st.st_gid, mode,
                 uid, gid);
}

static void create(const char *path, uint32_t mode)
{
    struct file *f;
    int r = vfs_open(path, O_WRONLY | O_CREAT | O_EXCL, mode, &f);
    ktest_assert(r == 0, "create %s: %d", path, r);
    file_put(f);
}

static void test_mfs_owners(void)
{
    ktest_assert(vfs_mkdir("/mnt", 0755) == 0 || vfs_mkdir("/mnt", 0755) == -EEXIST, "mkdir /mnt");
    ktest_assert(vfs_mount("mfs", "vdb", "/mnt", "uid=5") == -EINVAL, "mfs accepted an option");
    ktest_assert(vfs_mount("mfs", "vdb", "/mnt", NULL) == 0, "mount vdb");

    /* Root with umask 022. */
    create("/mnt/root.txt", 0666);
    expect("/mnt/root.txt", 0, 0644, 0, 0);
    ktest_assert(vfs_mkdir("/mnt/shared", 0777) == 0, "mkdir shared");
    expect("/mnt/shared", 0, 0755, 0, 0);
    ktest_assert(vfs_chown("/mnt/shared", 0, 50, 0) == 0 && vfs_chmod("/mnt/shared", 02775, 0) == 0,
                 "prepare the shared directory");
    /* The root of the volume is root's, mode 0755. uid 1000 needs write
     * permission there for the files below. */
    ktest_assert(vfs_chmod("/mnt", 0777, 0) == 0, "open the volume root");

    /* uid 1000 with umask 077 and the supplementary group 50. */
    uint32_t groups[1] = { 50 };
    become(1000, 1000, 077, 1, groups);
    create("/mnt/user.txt", 0666);
    expect("/mnt/user.txt", 0, 0600, 1000, 1000);
    ktest_assert(vfs_mkdir("/mnt/udir", 0777) == 0, "mkdir udir");
    expect("/mnt/udir", 0, 0700, 1000, 1000);
    ktest_assert(vfs_symlink("user.txt", "/mnt/ulink") == 0, "symlink");
    expect("/mnt/ulink", VFS_NOFOLLOW, 0777, 1000, 1000);

    /* A set group id directory hands its group and, to directories, the
     * bit itself to what is created in it. */
    create("/mnt/shared/f", 0640);
    expect("/mnt/shared/f", 0, 0600, 1000, 50);
    ktest_assert(vfs_mkdir("/mnt/shared/d", 0770) == 0, "mkdir shared/d");
    expect("/mnt/shared/d", 0, 02700, 1000, 50);

    /* The owner changes the mode, not another's. */
    ktest_assert(vfs_chmod("/mnt/user.txt", 0644, 0) == 0, "owner chmod");
    expect("/mnt/user.txt", 0, 0644, 1000, 1000);
    ktest_assert(vfs_chmod("/mnt/root.txt", 0666, 0) == -EPERM, "chmod of root's file");
    /* Only root gives a file away, and the owner may move it to its groups. */
    ktest_assert(vfs_chown("/mnt/user.txt", 0, VFS_CHOWN_KEEP, 0) == -EPERM, "chown to root");
    ktest_assert(vfs_chown("/mnt/user.txt", VFS_CHOWN_KEEP, 7, 0) == -EPERM, "chgrp to a foreign group");
    ktest_assert(vfs_chown("/mnt/user.txt", VFS_CHOWN_KEEP, 50, 0) == 0, "chgrp to an own group");
    expect("/mnt/user.txt", 0, 0644, 1000, 50);
    ktest_assert(vfs_chown("/mnt/root.txt", VFS_CHOWN_KEEP, 50, 0) == -EPERM, "chgrp of root's file");
    /* The set group id bit needs membership in the file's group. */
    ktest_assert(vfs_chown("/mnt/udir", VFS_CHOWN_KEEP, 50, 0) == 0, "chgrp udir");
    become(1000, 1000, 077, 0, NULL);
    ktest_assert(vfs_chmod("/mnt/udir", 02750, 0) == 0, "chmod udir");
    expect("/mnt/udir", 0, 0750, 1000, 50);
    /* lchown changes the link, not its target. */
    become(1000, 1000, 077, 1, groups);
    ktest_assert(vfs_chown("/mnt/ulink", VFS_CHOWN_KEEP, 50, VFS_NOFOLLOW) == 0, "lchown");
    expect("/mnt/ulink", VFS_NOFOLLOW, 0777, 1000, 50);

    /* A change by the owner drops the set id bits root gave the file. */
    become_root();
    ktest_assert(vfs_chmod("/mnt/user.txt", 06755, 0) == 0, "root sets the set id bits");
    expect("/mnt/user.txt", 0, 06755, 1000, 50);
    become(1000, 1000, 077, 1, groups);
    ktest_assert(vfs_chown("/mnt/user.txt", VFS_CHOWN_KEEP, 1000, 0) == 0, "owner chgrp");
    expect("/mnt/user.txt", 0, 0755, 1000, 1000);
    become_root();
    ktest_assert(vfs_chown("/mnt/root.txt", 1000, 1000, 0) == 0, "root gives a file away");

    /* Everything is on disk after a new mount. */
    ktest_assert(vfs_umount("/mnt") == 0, "umount");
    ktest_assert(vfs_mount("mfs", "vdb", "/mnt", NULL) == 0, "mount again");
    expect("/mnt/root.txt", 0, 0644, 1000, 1000);
    expect("/mnt/user.txt", 0, 0755, 1000, 1000);
    expect("/mnt/udir", 0, 0750, 1000, 50);
    expect("/mnt/ulink", VFS_NOFOLLOW, 0777, 1000, 50);
    expect("/mnt", 0, 0777, 0, 0);
    expect("/mnt/shared", 0, 02775, 0, 50);
    expect("/mnt/shared/f", 0, 0600, 1000, 50);
    expect("/mnt/shared/d", 0, 02700, 1000, 50);
    ktest_assert(vfs_umount("/mnt") == 0, "umount at the end");
}

static void test_fat_owners(void)
{
    ktest_assert(vfs_mount("fat", "vdc", "/mnt", "uid=1000,gid=100,umask=bad") == -EINVAL, "bad umask");
    ktest_assert(vfs_mount("fat", "vdc", "/mnt", "owner=1000") == -EINVAL, "unknown option");
    ktest_assert(vfs_mount("fat", "vdc", "/mnt", "uid=1000,gid=100,umask=027") == 0, "mount fat");
    expect("/mnt", 0, 0750, 1000, 100);
    create("/mnt/file.txt", 0644);
    expect("/mnt/file.txt", 0, 0640, 1000, 100);
    ktest_assert(vfs_mkdir("/mnt/dir", 0777) == 0, "fat mkdir");
    expect("/mnt/dir", 0, 0750, 1000, 100);
    /* The owner's write bit is the read only attribute, nothing else
     * changes. */
    ktest_assert(vfs_chmod("/mnt/file.txt", 0440, 0) == 0, "fat chmod read only");
    expect("/mnt/file.txt", 0, 0440, 1000, 100);
    ktest_assert(vfs_chmod("/mnt/file.txt", 0755, 0) == -EPERM, "fat chmod with execute bits");
    ktest_assert(vfs_chown("/mnt/file.txt", 0, 0, 0) == -EPERM, "fat chown");
    ktest_assert(vfs_umount("/mnt") == 0, "umount fat");
    ktest_assert(vfs_mount("fat", "vdc", "/mnt", NULL) == 0, "mount fat with defaults");
    expect("/mnt/file.txt", 0, 0444, 0, 0);
    expect("/mnt/dir", 0, 0755, 0, 0);
    ktest_assert(vfs_umount("/mnt") == 0, "umount fat again");
}

static void test_devfs_owners(void)
{
    expect("/dev/null", 0, 0666, 0, 0);
    ktest_assert(vfs_chown("/dev/null", 1000, 1000, 0) == 0, "devfs chown");
    expect("/dev/null", 0, 0666, 1000, 1000);
    become(1000, 1000, 022, 0, NULL);
    ktest_assert(vfs_chmod("/dev/null", 0600, 0) == 0, "devfs chmod by the owner");
    expect("/dev/null", 0, 0600, 1000, 1000);
    become_root();
    ktest_assert(vfs_chown("/dev/null", 0, 0, 0) == 0 && vfs_chmod("/dev/null", 0666, 0) == 0, "devfs restore");
    expect("/dev/null", 0, 0666, 0, 0);
}

static void test_owner(void)
{
    test_mfs_owners();
    test_fat_owners();
    test_devfs_owners();
}
KTEST_DEFINE("fs_owner", test_owner);
