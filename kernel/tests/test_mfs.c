#include <tests/ktest.h>
#include <fs/vfs.h>
#include <fs/mfs_format.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <console.h>
#include <errno.h>

/* M13: the mfs root on vda through the VFS: files spanning direct,
 * indirect and double indirect blocks, truncation, links, directories,
 * rename, and block accounting. */
static uint64_t free_blocks(void)
{
    ktest_assert(vfs_sync() == 0, "sync before reading the superblock");
    struct blockdev *dev = blockdev_find("vda");
    struct buf *b = bread(dev, 0);
    ktest_assert(b != NULL, "read superblock");
    struct mfs_superblock sb;
    memcpy(&sb, b->data, sizeof sb);
    brelse(b);
    return sb.free_blocks;
}

static uint32_t sb_flags(void)
{
    struct blockdev *dev = blockdev_find("vda");
    struct buf *b = bread(dev, 0);
    ktest_assert(b != NULL, "read superblock");
    uint32_t flags = ((struct mfs_superblock *)b->data)->flags;
    brelse(b);
    return flags;
}

static void write_all(struct file *f, const char *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        long r = file_write(f, buf + done, n - done);
        ktest_assert(r > 0, "write returned %ld", r);
        done += (size_t)r;
    }
}

static void read_all(struct file *f, char *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        long r = file_read(f, buf + done, n - done);
        ktest_assert(r > 0, "read returned %ld at %zu", r, done);
        done += (size_t)r;
    }
}

static void test_mfs(void)
{
    struct inode *root;
    ktest_assert(vfs_lookup("/", &root) == 0, "lookup root");
    ktest_assert(strcmp(root->sb->type->name, "mfs") == 0, "root is %s, not mfs", root->sb->type->name);
    struct fs_space space;
    ktest_assert(root->sb->ops->statfs && root->sb->ops->statfs(root->sb, &space) == 0, "mfs statfs");
    ktest_assert(space.block_size == MFS_BLOCK_SIZE && space.free_blocks == free_blocks() &&
                 space.blocks > space.free_blocks, "mfs space accounting");
    inode_put(root);
    ktest_assert(!(sb_flags() & MFS_FLAG_CLEAN), "clean flag not cleared on mount");
    ktest_assert(vfs_sync() == 0, "sync");
    uint64_t free0 = free_blocks();
    kprintf("mfs: %lu free blocks at start\n", free0);

    /* A large file: 4.5 MiB needs direct, indirect and double indirect
     * blocks. Verify a pattern that depends on the offset. */
    struct file *f;
    ktest_assert(vfs_open("/big", O_RDWR | O_CREAT | O_EXCL, 0644, &f) == 0, "create /big");
    const size_t big = 4608 * 1024;
    char *chunk = kmalloc(65536);
    ktest_assert(chunk != NULL, "alloc");
    for (size_t off = 0; off < big; off += 65536) {
        for (size_t i = 0; i < 65536; i++)
            chunk[i] = (char)((off + i) * 31 >> 8);
        write_all(f, chunk, 65536);
    }
    ktest_assert(f->inode->size == big, "size %lu", f->inode->size);
    /* The modification time follows the real time clock. */
    int64_t now = vfs_now();
    ktest_assert(f->inode->mtime > 1600000000LL * 1000000000 && now - f->inode->mtime < 30LL * 1000000000, "mtime %ld at %ld", f->inode->mtime, now);
    ktest_assert(file_lseek(f, 0, SEEK_SET) == 0, "rewind");
    for (size_t off = 0; off < big; off += 65536) {
        read_all(f, chunk, 65536);
        for (size_t i = 0; i < 65536; i += 97)
            ktest_assert(chunk[i] == (char)((off + i) * 31 >> 8), "mismatch at %zu", off + i);
    }
    ktest_assert(file_read(f, chunk, 10) == 0, "eof");
    file_put(f);
    uint64_t used = free0 - free_blocks();
    kprintf("mfs: /big uses %lu blocks\n", used);
    ktest_assert(used == big / 4096 + 1 + 1 + 1, "block accounting %lu", used);

    /* Truncate through O_TRUNC releases everything. */
    ktest_assert(vfs_open("/big", O_WRONLY | O_TRUNC, 0, &f) == 0, "truncate /big");
    ktest_assert(f->inode->size == 0, "size after truncate");
    file_put(f);
    ktest_assert(free_blocks() == free0, "blocks after truncate: %lu vs %lu", free_blocks(), free0);

    /* Holes: write far beyond the end, read zeros in between. */
    ktest_assert(vfs_open("/big", O_RDWR, 0, &f) == 0, "reopen /big");
    ktest_assert(file_lseek(f, 100000, SEEK_SET) == 100000, "seek hole");
    write_all(f, "end", 3);
    ktest_assert(file_lseek(f, 50000, SEEK_SET) == 50000, "seek into hole");
    memset(chunk, 1, 100);
    ktest_assert(file_read(f, chunk, 100) == 100 && chunk[0] == 0 && chunk[99] == 0, "hole reads zero");
    ktest_assert(f->inode->size == 100003, "size with hole %lu", f->inode->size);
    file_put(f);
    ktest_assert(vfs_unlink("/big") == 0, "unlink /big");
    ktest_assert(free_blocks() == free0, "blocks after unlink: %lu vs %lu", free_blocks(), free0);
    ktest_assert(vfs_lookup("/big", &root) == -ENOENT, "gone");

    /* Directories and names. */
    ktest_assert(vfs_mkdir("/d") == 0, "mkdir /d");
    ktest_assert(vfs_mkdir("/d") == -EEXIST, "mkdir twice");
    ktest_assert(vfs_mkdir("/d/sub") == 0, "mkdir /d/sub");
    ktest_assert(vfs_open("/d/sub/file", O_WRONLY | O_CREAT, 0644, &f) == 0, "create nested");
    write_all(f, "hello", 5);
    file_put(f);
    ktest_assert(vfs_rmdir("/d") == -ENOTEMPTY, "rmdir non empty");
    ktest_assert(vfs_rmdir("/d/sub/file") == -ENOTDIR, "rmdir file");
    ktest_assert(vfs_unlink("/d/sub") == -EISDIR, "unlink dir");
    struct inode *d;
    ktest_assert(vfs_lookup("/d", &d) == 0 && d->nlink == 3, "nlink of /d is %u", d->nlink);
    inode_put(d);

    /* Hard links share content; the data survives until the last name goes. */
    ktest_assert(vfs_link("/d/sub/file", "/d/link") == 0, "link");
    ktest_assert(vfs_lookup("/d/link", &d) == 0 && d->nlink == 2 && d->size == 5, "linked inode");
    inode_put(d);
    ktest_assert(vfs_unlink("/d/sub/file") == 0, "unlink original");
    ktest_assert(vfs_open("/d/link", O_RDONLY, 0, &f) == 0, "open via link");
    ktest_assert(file_read(f, chunk, 10) == 5 && memcmp(chunk, "hello", 5) == 0, "content via link");
    ktest_assert(f->inode->nlink == 1, "nlink after unlink");
    file_put(f);

    /* Rename: within a directory, across directories, over an existing file. */
    ktest_assert(vfs_rename("/d/link", "/d/renamed") == 0, "rename same dir");
    ktest_assert(vfs_lookup("/d/link", &d) == -ENOENT, "old name gone");
    ktest_assert(vfs_rename("/d/renamed", "/d/sub/moved") == 0, "rename across dirs");
    ktest_assert(vfs_open("/d/other", O_WRONLY | O_CREAT, 0644, &f) == 0, "create other");
    write_all(f, "other", 5);
    file_put(f);
    uint64_t before_replace = free_blocks();
    ktest_assert(vfs_rename("/d/sub/moved", "/d/other") == 0, "rename over file");
    ktest_assert(free_blocks() == before_replace + 1, "replaced file blocks freed");
    ktest_assert(vfs_open("/d/other", O_RDONLY, 0, &f) == 0 && file_read(f, chunk, 10) == 5 &&
                 memcmp(chunk, "hello", 5) == 0, "content after replace");
    file_put(f);
    ktest_assert(vfs_rename("/d/sub", "/moved_dir") == 0, "rename directory across dirs");
    ktest_assert(vfs_lookup("/d", &d) == 0 && d->nlink == 2, "nlink of /d after move is %u", d->nlink);
    inode_put(d);
    ktest_assert(vfs_lookup("/moved_dir/..", &d) == 0 && d->ino == MFS_ROOT_INO, "dotdot updated");
    inode_put(d);
    ktest_assert(vfs_rmdir("/moved_dir") == 0, "rmdir moved dir");

    /* Directory listing through getdents. */
    ktest_assert(vfs_open("/d", O_RDONLY | O_DIRECTORY, 0, &f) == 0, "open /d");
    struct dirent *ents = kmalloc(8 * sizeof *ents);
    long n = file_getdents(f, ents, 8 * sizeof *ents);
    ktest_assert(n == 3 * (long)sizeof *ents, "getdents %ld bytes", n);
    ktest_assert(strcmp(ents[0].d_name, ".") == 0 && strcmp(ents[1].d_name, "..") == 0 &&
                 strcmp(ents[2].d_name, "other") == 0 && ents[2].d_type == DT_REG, "entries");
    ktest_assert(file_getdents(f, ents, 8 * sizeof *ents) == 0, "getdents end");
    file_put(f);
    kfree(ents);

    /* Clean up and check that nothing leaked on disk. */
    ktest_assert(vfs_unlink("/d/other") == 0, "unlink other");
    ktest_assert(vfs_rmdir("/d") == 0, "rmdir /d");
    ktest_assert(vfs_lookup("/d", &d) == -ENOENT, "/d gone");
    ktest_assert(vfs_sync() == 0, "final sync");
    ktest_assert(free_blocks() == free0, "blocks at end: %lu vs %lu", free_blocks(), free0);
    ktest_assert(bcache_dirty_count() == 0, "dirty buffers after sync");

    /* Programs on the disk root run. */
    struct inode *sh;
    ktest_assert(vfs_lookup("/bin/sh", &sh) == 0 && S_ISREG(sh->mode) && sh->size > 1000, "/bin/sh on disk");
    ktest_assert(sh->mtime > 1600000000LL * 1000000000, "mkfs kept the host time: %ld", sh->mtime);
    inode_put(sh);
    ktest_assert(vfs_lookup("/initrd/bin/sh", &sh) == 0, "initrd on /initrd");
    inode_put(sh);
    kfree(chunk);
}
KTEST_DEFINE("mfs", test_mfs);
