#include <tests/ktest.h>
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <console.h>
#include <errno.h>
#include "../fs/fat/fat.h"

/* M36: FAT12, FAT16 and FAT32 images built by mkfat on vdb, vdc and vdd,
 * each with the same tree. Reads the tree with long names, then creates,
 * writes, renames, truncates and removes, checks the cluster accounting
 * and leaves a file for the host tool to read. */

static void write_all(struct file *f, const char *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        long r = file_write(f, buf + done, n - done);
        ktest_assert(r > 0, "write returned %ld", r);
        done += (size_t)r;
    }
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

static int count_entries(const char *path, const char *want)
{
    struct file *f;
    ktest_assert(vfs_open(path, O_RDONLY | O_DIRECTORY, 0, &f) == 0, "open %s", path);
    struct dirent *ents = kmalloc(8 * sizeof *ents);
    int n = 0;
    bool found = false;
    long r;
    while ((r = file_getdents(f, ents, 8 * sizeof *ents)) > 0) {
        for (long i = 0; i < r / (long)sizeof *ents; i++) {
            n++;
            if (want && strcmp(ents[i].d_name, want) == 0)
                found = true;
        }
    }
    kfree(ents);
    file_put(f);
    ktest_assert(!want || found, "%s not listed in %s", want, path);
    return n;
}

static struct fat_sb *fat_sb_of(const char *path)
{
    struct inode *i;
    ktest_assert(vfs_lookup(path, &i) == 0, "lookup %s", path);
    struct fat_sb *m = i->sb->priv;
    ktest_assert(strcmp(i->sb->type->name, "fat") == 0, "%s is not fat", path);
    inode_put(i);
    return m;
}

static void exercise(const char *dev, int type)
{
    int r = vfs_mount("fat", dev, "/mnt", NULL);
    ktest_assert(r == 0, "mount %s: %d", dev, r);
    struct fat_sb *m = fat_sb_of("/mnt");
    ktest_assert(m->type == type, "%s: type %d, expected %d", dev, m->type, type);
    ktest_assert(m->free_clusters == fat_count_free(m), "free count at mount");
    uint32_t free0 = m->free_clusters;
    struct inode *root;
    ktest_assert(vfs_lookup("/mnt", &root) == 0, "statfs root");
    struct fs_space space;
    ktest_assert(root->sb->ops->statfs && root->sb->ops->statfs(root->sb, &space) == 0, "fat statfs");
    ktest_assert(space.blocks == m->nclusters && space.free_blocks == free0 &&
                 space.block_size == m->cluster_bytes, "FAT%d space accounting", type);
    inode_put(root);

    /* The tree written by mkfat, with long names and case folding. */
    static char big[20000];
    for (int i = 0; i < 20000; i++)
        big[i] = (char)((i * 7) % 251);
    ktest_assert(file_holds("/mnt/readme.txt", "hello\n", 6), "readme.txt");
    ktest_assert(file_holds("/mnt/README.TXT", "hello\n", 6), "README.TXT (case)");
    ktest_assert(file_holds("/mnt/lower.txt", "lower\n", 6), "lower.txt");
    ktest_assert(file_holds("/mnt/\xc3\x9c" "bersicht.txt", "\xc3\xbc\n", 3), "Übersicht.txt");
    ktest_assert(file_holds("/mnt/Long Directory Name/nested/big data file.bin", big, sizeof big), "big data file");
    ktest_assert(file_holds("/mnt/long directory name/NESTED/Big Data File.BIN", big, sizeof big), "case folded path");
    ktest_assert(count_entries("/mnt", "Long Directory Name") == 6, "root listing");
    ktest_assert(count_entries("/mnt/Long Directory Name/nested", "big data file.bin") == 3, "nested listing");
    ktest_assert(!exists("/mnt/missing"), "missing");

    /* Create with a long name and write 100000 bytes. */
    static char pattern[100000];
    for (size_t i = 0; i < sizeof pattern; i++)
        pattern[i] = i % 26 == 0 ? ' ' : (char)('a' + i % 26);
    ktest_assert(vfs_mkdir("/mnt/kept", 0777) == 0, "mkdir kept");
    ktest_assert(vfs_mkdir("/mnt/kept", 0777) == -EEXIST, "mkdir twice");
    struct file *f;
    r = vfs_open("/mnt/kept/Written by the kernel.txt", O_RDWR | O_CREAT | O_EXCL, 0644, &f);
    ktest_assert(r == 0, "create: %d", r);
    ksnprintf(pattern, 32, "kernel pattern %010u", 0u);
    pattern[25] = 'x';
    write_all(f, pattern, sizeof pattern);
    ktest_assert(f->inode->size == sizeof pattern, "size %lu", f->inode->size);
    ktest_assert(f->inode->mtime > 1600000000LL * 1000000000 && vfs_now() - f->inode->mtime < 30LL * 1000000000, "mtime %ld", f->inode->mtime);
    file_put(f);
    ktest_assert(file_holds("/mnt/kept/written BY the KERNEL.txt", pattern, sizeof pattern), "read back");
    /* The time survives the directory entry: read the inode again. */
    struct inode *written;
    ktest_assert(vfs_lookup("/mnt/kept/Written by the kernel.txt", &written) == 0, "lookup written");
    ktest_assert(written->mtime > 1600000000LL * 1000000000, "entry time %ld", written->mtime);
    inode_put(written);
    ktest_assert(vfs_open("/mnt/kept/Written by the kernel.txt", O_WRONLY | O_CREAT | O_EXCL, 0644, &f) == -EEXIST,
                 "O_EXCL");
    uint32_t per_file = (sizeof pattern + m->cluster_bytes - 1) / m->cluster_bytes;
    ktest_assert(m->free_clusters == free0 - 1 - per_file, "clusters after create: %u, expected %u",
                 m->free_clusters, free0 - 1 - per_file);

    /* Short names must be unique: two long names with the same prefix. */
    ktest_assert(vfs_open("/mnt/kept/removed file one.txt", O_WRONLY | O_CREAT, 0644, &f) == 0, "create one");
    write_all(f, "one", 3);
    file_put(f);
    ktest_assert(vfs_open("/mnt/kept/removed file two.txt", O_WRONLY | O_CREAT, 0644, &f) == 0, "create two");
    write_all(f, "two", 3);
    file_put(f);
    ktest_assert(file_holds("/mnt/kept/removed file one.txt", "one", 3) &&
                 file_holds("/mnt/kept/removed file two.txt", "two", 3), "distinct files");
    struct inode *a, *b;
    ktest_assert(vfs_lookup("/mnt/kept/removed file one.txt", &a) == 0 &&
                 vfs_lookup("/mnt/kept/removed file two.txt", &b) == 0 && a->ino != b->ino, "distinct inodes");
    inode_put(a);
    inode_put(b);

    /* Rename within and across directories, over an existing file. */
    ktest_assert(vfs_rename("/mnt/kept/removed file one.txt", "/mnt/kept/removed file 1.txt") == 0, "rename");
    ktest_assert(!exists("/mnt/kept/removed file one.txt") && file_holds("/mnt/kept/removed file 1.txt", "one", 3),
                 "renamed");
    ktest_assert(vfs_rename("/mnt/kept/removed file 1.txt", "/mnt/removed elsewhere.txt") == 0, "rename across");
    ktest_assert(file_holds("/mnt/removed elsewhere.txt", "one", 3), "moved");
    ktest_assert(vfs_rename("/mnt/removed elsewhere.txt", "/mnt/kept/removed file two.txt") == 0, "rename over");
    ktest_assert(file_holds("/mnt/kept/removed file two.txt", "one", 3), "replaced");
    ktest_assert(vfs_mkdir("/mnt/removed dir", 0777) == 0, "mkdir removed dir");
    ktest_assert(vfs_rename("/mnt/removed dir", "/mnt/kept/removed dir") == 0, "rename directory");
    ktest_assert(vfs_open("/mnt/kept/removed dir/inner", O_WRONLY | O_CREAT, 0644, &f) == 0, "create inner");
    file_put(f);
    ktest_assert(vfs_rmdir("/mnt/kept/removed dir") == -ENOTEMPTY, "rmdir non empty");
    ktest_assert(vfs_unlink("/mnt/kept/removed dir") == -EISDIR, "unlink dir");
    ktest_assert(vfs_rmdir("/mnt/kept/removed dir/inner") == -ENOTDIR, "rmdir file");

    /* Truncate, append, unlink with the file open. */
    ktest_assert(vfs_open("/mnt/kept/removed file two.txt", O_WRONLY | O_TRUNC, 0, &f) == 0, "truncate");
    ktest_assert(f->inode->size == 0, "size after truncate");
    write_all(f, "again", 5);
    file_put(f);
    ktest_assert(vfs_open("/mnt/kept/removed file two.txt", O_WRONLY | O_APPEND, 0, &f) == 0, "append");
    write_all(f, " more", 5);
    file_put(f);
    ktest_assert(file_holds("/mnt/kept/removed file two.txt", "again more", 10), "appended");
    ktest_assert(vfs_open("/mnt/kept/removed file two.txt", O_RDONLY, 0, &f) == 0, "open for unlink");
    ktest_assert(vfs_unlink("/mnt/kept/removed file two.txt") == 0, "unlink open file");
    ktest_assert(!exists("/mnt/kept/removed file two.txt"), "name gone");
    char tmp[16];
    ktest_assert(file_read(f, tmp, 16) == 10 && memcmp(tmp, "again more", 10) == 0, "data until close");
    file_put(f);
    ktest_assert(vfs_unlink("/mnt/kept/removed dir/inner") == 0, "unlink inner");
    ktest_assert(vfs_rmdir("/mnt/kept/removed dir") == 0, "rmdir");
    ktest_assert(m->free_clusters == free0 - 1 - per_file, "clusters after cleanup: %u, expected %u",
                 m->free_clusters, free0 - 1 - per_file);
    ktest_assert(m->free_clusters == fat_count_free(m), "free count agrees with the table");
    ktest_assert(count_entries("/mnt/kept", "Written by the kernel.txt") == 3, "kept listing");

    /* Persistence across a remount. */
    ktest_assert(vfs_sync() == 0, "sync");
    r = vfs_umount("/mnt");
    ktest_assert(r == 0, "umount: %d", r);
    ktest_assert(vfs_mount("fat", dev, "/mnt", NULL) == 0, "remount");
    m = fat_sb_of("/mnt");
    ktest_assert(m->free_clusters == free0 - 1 - per_file, "clusters after remount");
    ktest_assert(file_holds("/mnt/kept/Written by the kernel.txt", pattern, sizeof pattern), "kept after remount");
    ktest_assert(file_holds("/mnt/Long Directory Name/nested/big data file.bin", big, sizeof big), "tree after remount");
    ktest_assert(vfs_umount("/mnt") == 0, "umount again");
    kprintf("fat: %s FAT%d ok\n", dev, type);
}

static void test_fat(void)
{
    ktest_assert(vfs_mkdir("/mnt", 0777) == 0, "mkdir /mnt");
    ktest_assert(vfs_mount("fat", "vda", "/mnt", NULL) == -EINVAL, "mfs image is not FAT");
    exercise("vdb", 12);
    exercise("vdc", 16);
    exercise("vdd", 32);
    ktest_assert(bcache_dirty_count() == 0, "dirty buffers after unmount");
}
KTEST_DEFINE("fat", test_fat);
