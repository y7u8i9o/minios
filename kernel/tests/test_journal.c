#include <tests/ktest.h>
#include <fs/vfs.h>
#include <fs/mfs_format.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <sched/thread.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <console.h>
#include <lib/printf.h>
#include <errno.h>

/* M36: the mfs journal. A second mfs image on vdb is mounted on /mnt and
 * taken through simulated crashes: after a commit the journal must bring
 * the metadata back, before a commit nothing of the operation may
 * survive, and the bitmaps and counters must agree afterwards. Finally
 * a committed but not checkpointed transaction is left on the root image
 * for the host fsck tool to replay. */

void mfs_journal_set_crash(struct superblock *sb, int mode);
#define CRASH_BEFORE_COMMIT 1
#define CRASH_AFTER_COMMIT  2

static struct superblock *sb_of(const char *path)
{
    struct inode *i;
    ktest_assert(vfs_lookup(path, &i) == 0, "lookup %s", path);
    struct superblock *sb = i->sb;
    inode_put(i);
    return sb;
}

static void write_file(const char *path, const char *data, size_t n)
{
    struct file *f;
    ktest_assert(vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644, &f) == 0, "create %s", path);
    size_t done = 0;
    while (done < n) {
        long r = file_write(f, data + done, n - done);
        ktest_assert(r > 0, "write %s: %ld", path, r);
        done += (size_t)r;
    }
    file_put(f);
}

static bool file_holds(const char *path, const char *data, size_t n)
{
    struct file *f;
    if (vfs_open(path, O_RDONLY, 0, &f) < 0)
        return false;
    char *buf = kmalloc(n + 1);
    size_t done = 0;
    while (done < n) {
        long r = file_read(f, buf + done, n - done);
        if (r <= 0)
            break;
        done += (size_t)r;
    }
    bool ok = done == n && memcmp(buf, data, n) == 0 && file_read(f, buf, 1) == 0;
    kfree(buf);
    file_put(f);
    return ok;
}

static bool exists(const char *path)
{
    struct inode *i;
    if (vfs_lookup(path, &i) < 0)
        return false;
    inode_put(i);
    return true;
}

/* Read the superblock and count the free bits of both bitmaps directly
 * from the disk, after a sync. */
static void disk_counts(struct blockdev *dev, uint64_t *free_blocks, uint64_t *free_bits,
                        uint32_t *free_inodes, uint64_t *free_inode_bits)
{
    struct buf *b = bread(dev, 0);
    ktest_assert(b != NULL, "read superblock");
    struct mfs_superblock sb;
    memcpy(&sb, b->data, sizeof sb);
    brelse(b);
    *free_blocks = sb.free_blocks;
    *free_inodes = sb.free_inodes;
    *free_bits = 0;
    for (uint64_t bit = sb.data_start; bit < sb.nblocks; bit++) {
        b = bread(dev, sb.block_bitmap_start + bit / (MFS_BLOCK_SIZE * 8));
        ktest_assert(b != NULL, "read bitmap");
        *free_bits += !(b->data[(bit / 8) % MFS_BLOCK_SIZE] >> (bit % 8) & 1);
        brelse(b);
    }
    *free_inode_bits = 0;
    for (uint64_t bit = 1; bit < sb.ninodes; bit++) {
        b = bread(dev, sb.inode_bitmap_start + bit / (MFS_BLOCK_SIZE * 8));
        ktest_assert(b != NULL, "read inode bitmap");
        *free_inode_bits += !(b->data[(bit / 8) % MFS_BLOCK_SIZE] >> (bit % 8) & 1);
        brelse(b);
    }
}

static void check_consistent(struct blockdev *dev, const char *when)
{
    ktest_assert(vfs_sync() == 0, "sync");
    uint64_t free_blocks, free_bits, free_inode_bits;
    uint32_t free_inodes;
    disk_counts(dev, &free_blocks, &free_bits, &free_inodes, &free_inode_bits);
    kprintf("journal: %s: %lu free blocks, %lu free inodes\n", when, free_blocks, free_inode_bits);
    ktest_assert(free_blocks == free_bits, "%s: free block count %lu, bitmap %lu", when, free_blocks, free_bits);
    ktest_assert(free_inodes == free_inode_bits, "%s: free inode count %u, bitmap %lu", when, free_inodes,
                 free_inode_bits);
}

static void remount(struct superblock *sb, int crash)
{
    mfs_journal_set_crash(sb, crash);
    int r = vfs_umount("/mnt");
    ktest_assert(r == 0, "umount /mnt: %d", r);
    r = vfs_mount("mfs", "vdb", "/mnt");
    ktest_assert(r == 0, "mount vdb again: %d", r);
}

/* Concurrent operations share transactions and wait for journal space. */
struct worker_arg {
    int id;
    int errors;
};

static void worker(void *arg)
{
    struct worker_arg *w = arg;
    char path[64], data[2000];
    for (int i = 0; i < 12; i++) {
        ksnprintf(path, sizeof path, "/mnt/dir/w%d_%d", w->id, i);
        memset(data, 'a' + w->id, sizeof data);
        struct file *f;
        if (vfs_open(path, O_WRONLY | O_CREAT, 0644, &f) < 0) {
            w->errors++;
            continue;
        }
        if (file_write(f, data, sizeof data) != (long)sizeof data)
            w->errors++;
        file_put(f);
        if (i % 3 == 2 && vfs_unlink(path) < 0)
            w->errors++;
    }
    thread_exit(0);
}

static void test_journal(void)
{
    struct blockdev *dev = blockdev_find("vdb");
    ktest_assert(dev != NULL, "vdb present");
    ktest_assert(vfs_mkdir("/mnt") == 0, "mkdir /mnt");
    int r = vfs_mount("mfs", "vdb", "/mnt");
    ktest_assert(r == 0, "mount vdb: %d", r);
    struct superblock *sb = sb_of("/mnt");
    ktest_assert(strcmp(sb->type->name, "mfs") == 0 && sb != sb_of("/"), "/mnt is a second mfs");

    static char pattern[20000];
    for (size_t i = 0; i < sizeof pattern; i++)
        pattern[i] = (char)('A' + (i * 7) % 26);
    write_file("/mnt/base.txt", "base", 4);
    ktest_assert(vfs_mkdir("/mnt/dir") == 0, "mkdir /mnt/dir");
    check_consistent(dev, "after setup");

    /* A crash after the commit: the journal restores every change. */
    mfs_journal_set_crash(sb, CRASH_AFTER_COMMIT);
    write_file("/mnt/dir/after.txt", pattern, sizeof pattern);
    ktest_assert(vfs_mkdir("/mnt/dir/sub") == 0, "mkdir sub");
    ktest_assert(vfs_unlink("/mnt/base.txt") == 0, "unlink base");
    remount(sb, CRASH_AFTER_COMMIT);
    sb = sb_of("/mnt");
    ktest_assert(file_holds("/mnt/dir/after.txt", pattern, sizeof pattern), "after.txt restored by the journal");
    ktest_assert(exists("/mnt/dir/sub"), "sub restored by the journal");
    ktest_assert(!exists("/mnt/base.txt"), "base.txt removal restored by the journal");
    check_consistent(dev, "after replay");

    /* A crash before the commit: nothing of the operations survives. */
    mfs_journal_set_crash(sb, CRASH_BEFORE_COMMIT);
    write_file("/mnt/lost.txt", pattern, 5000);
    ktest_assert(vfs_rename("/mnt/dir/after.txt", "/mnt/dir/moved.txt") == 0, "rename");
    remount(sb, CRASH_BEFORE_COMMIT);
    sb = sb_of("/mnt");
    ktest_assert(!exists("/mnt/lost.txt"), "lost.txt did not survive");
    ktest_assert(file_holds("/mnt/dir/after.txt", pattern, sizeof pattern), "after.txt unchanged");
    ktest_assert(!exists("/mnt/dir/moved.txt"), "rename did not survive");
    check_consistent(dev, "after the lost transaction");

    /* Concurrent operations. */
    struct worker_arg args[8];
    struct thread *threads[8];
    for (int i = 0; i < 8; i++) {
        args[i].id = i;
        args[i].errors = 0;
        threads[i] = thread_create("jworker", worker, &args[i], 0);
        ktest_assert(threads[i] != NULL, "thread_create");
    }
    for (int i = 0; i < 8; i++) {
        thread_join(threads[i]);
        ktest_assert(args[i].errors == 0, "worker %d: %d errors", i, args[i].errors);
    }
    struct file *f;
    ktest_assert(vfs_open("/mnt/dir", O_RDONLY | O_DIRECTORY, 0, &f) == 0, "open /mnt/dir");
    struct dirent *ents = kmalloc(16 * sizeof *ents);
    int entries = 0;
    long n;
    while ((n = file_getdents(f, ents, 16 * sizeof *ents)) > 0)
        entries += (int)(n / (long)sizeof *ents);
    file_put(f);
    kfree(ents);
    ktest_assert(entries == 2 + 2 + 8 * 8, "entries in /mnt/dir: %d", entries);
    check_consistent(dev, "after the workers");

    /* Clean unmount for the host check of the second image. */
    ktest_assert(vfs_umount("/mnt") == 0, "final umount");
    ktest_assert(vfs_mount("mfs", "vdb", "/mnt") == 0, "mount after clean unmount");
    ktest_assert(file_holds("/mnt/dir/after.txt", pattern, sizeof pattern), "after.txt after clean remount");
    ktest_assert(vfs_umount("/mnt") == 0, "umount again");

    /* Leave a committed transaction on the root image for fsck. */
    struct superblock *root = sb_of("/");
    mfs_journal_set_crash(root, CRASH_AFTER_COMMIT);
    write_file("/journaled.txt", "replayed by fsck\n", 17);
    kprintf("journal: root left with a committed transaction\n");
}
KTEST_DEFINE("mfs_journal", test_journal);
