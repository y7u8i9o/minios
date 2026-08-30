#include <tests/ktest.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <fs/vfs.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <console.h>
#include <drivers/timer.h>

/* M12: raw sector transfers through virtio-blk, the block cache and the
 * /dev/vda file. The case boots with root=initrd so the disk contents
 * may be overwritten freely. */
static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + i * 7);
}

static bool check(const uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != (uint8_t)(seed + i * 7))
            return false;
    return true;
}

static void test_blk(void)
{
    struct blockdev *dev = blockdev_find("vda");
    ktest_assert(dev != NULL, "no vda");
    ktest_assert(dev->sector_size == 512 && dev->nsectors == 512 * 2048, "vda geometry %lu", dev->nsectors);

    /* Raw single and multi sector transfers, including one larger than
     * the 8 KiB bounce chunk. */
    uint8_t *w = kmalloc(65536), *r = kmalloc(65536);
    ktest_assert(w && r, "alloc");
    fill(w, 512, 1);
    ktest_assert(blockdev_write(dev, 3, 1, w) == 0, "write sector 3");
    memset(r, 0, 512);
    ktest_assert(blockdev_read(dev, 3, 1, r) == 0 && check(r, 512, 1), "read sector 3");
    fill(w, 65536, 2);
    ktest_assert(blockdev_write(dev, 100, 128, w) == 0, "write 128 sectors");
    memset(r, 0, 65536);
    ktest_assert(blockdev_read(dev, 100, 128, r) == 0 && check(r, 65536, 2), "read 128 sectors");
    memset(r, 0xaa, 512);
    ktest_assert(blockdev_read(dev, dev->nsectors - 1000, 1, r) == 0 && r[0] == 0 && r[511] == 0, "unused sector is zero");
    ktest_assert(blockdev_read(dev, dev->nsectors, 1, r) < 0, "read past end rejected");

    /* Block cache: writes stay in memory until sync. Block 5 may hold
     * filesystem data, so the check compares against its previous
     * contents rather than expecting zeros. */
    uint8_t *before = kmalloc(BCACHE_BLOCK_SIZE);
    ktest_assert(before && blockdev_read(dev, 40, 8, before) == 0, "read block 5");
    struct buf *b = bread(dev, 5);
    ktest_assert(b != NULL, "bread 5");
    fill(b->data, BCACHE_BLOCK_SIZE, 3);
    bwrite(b);
    brelse(b);
    ktest_assert(bcache_dirty_count() == 1, "dirty count %zu", bcache_dirty_count());
    ktest_assert(blockdev_read(dev, 40, 8, r) == 0 && memcmp(r, before, BCACHE_BLOCK_SIZE) == 0,
                 "write back is delayed");
    kfree(before);
    b = bread(dev, 5);
    ktest_assert(b && check(b->data, BCACHE_BLOCK_SIZE, 3), "cached data");
    brelse(b);
    ktest_assert(bcache_sync(dev) == 0 && bcache_dirty_count() == 0, "sync");
    ktest_assert(blockdev_read(dev, 40, 8, r) == 0 && check(r, BCACHE_BLOCK_SIZE, 3), "on disk after sync");

    /* Eviction: touch more blocks than the cache holds, then find the
     * data still intact when it is read back from disk. */
    for (uint64_t blk = 1000; blk < 1000 + BCACHE_NBUF + 8; blk++) {
        b = bread(dev, blk);
        ktest_assert(b != NULL, "bread %lu", blk);
        b->data[0] = (uint8_t)blk;
        bwrite(b);
        brelse(b);
    }
    b = bread(dev, 5);
    ktest_assert(b && check(b->data, BCACHE_BLOCK_SIZE, 3), "block 5 after eviction");
    brelse(b);
    ktest_assert(bcache_sync(NULL) == 0, "sync all");
    b = bread(dev, 1100);
    ktest_assert(b && b->data[0] == (uint8_t)1100, "evicted dirty block written back");
    brelse(b);

    /* /dev/vda through the VFS. */
    struct file *f;
    ktest_assert(vfs_open("/dev/vda", O_RDWR, 0, &f) == 0, "open /dev/vda");
    ktest_assert(f->inode->size == 512 * 1024 * 1024 && S_ISBLK(f->inode->mode), "vda inode");
    ktest_assert(file_lseek(f, 5 * BCACHE_BLOCK_SIZE + 100, SEEK_SET) == 5 * BCACHE_BLOCK_SIZE + 100, "lseek");
    ktest_assert(file_read(f, (char *)r, 1000) == 1000 && check(r, 1000, 3 + 100 * 7), "read via file");
    ktest_assert(file_lseek(f, 7 * BCACHE_BLOCK_SIZE - 10, SEEK_SET) > 0, "lseek 2");
    fill(w, 100, 9);
    ktest_assert(file_write(f, (const char *)w, 100) == 100, "write across blocks");
    ktest_assert(file_lseek(f, -100, SEEK_CUR) > 0 && file_read(f, (char *)r, 100) == 100 && check(r, 100, 9),
                 "read back across blocks");
    ktest_assert(file_lseek(f, 0, SEEK_END) == 512 * 1024 * 1024 && file_read(f, (char *)r, 10) == 0, "eof");
    file_put(f);
    ktest_assert(vfs_sync() == 0, "vfs_sync");
    ktest_assert(blockdev_read(dev, 7 * 8 - 1, 1, r) == 0 && check(r + 502, 10, 9), "file write reached disk");
    /* Throughput of small transfers, for the swap design. */
    uint64_t t0 = timer_ms();
    for (int i = 0; i < 1000; i++)
        ktest_assert(blockdev_read(dev, (uint64_t)(i * 8), 8, r) == 0, "timed read %d", i);
    uint64_t t1 = timer_ms();
    for (int i = 0; i < 1000; i++)
        ktest_assert(blockdev_write(dev, (uint64_t)(i * 8), 8, r) == 0, "timed write %d", i);
    uint64_t t2 = timer_ms();
    kprintf("blk: 1000 x 4 KiB reads in %lu ms, writes in %lu ms\n", t1 - t0, t2 - t1);
    kfree(w);
    kfree(r);
}
KTEST_DEFINE("blk", test_blk);
