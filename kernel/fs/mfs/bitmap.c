#define KLOG_SUBSYS "mfs"
#include "mfs.h"
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>

#define BITS_PER_BLOCK (MFS_BLOCK_SIZE * 8)

/* Bitmap blocks and the superblock counters are metadata: every change
 * goes through the journal (M36). */

/* Find and set the first clear bit in [first, limit) of the bitmap that
 * starts at block start. Returns the bit or 0. */
static uint64_t bitmap_alloc(struct mfs_sb *m, uint32_t start, uint32_t nblocks,
                             uint64_t first, uint64_t limit)
{
    kassert(mutex_held(&m->lock));
    for (uint32_t b = (uint32_t)(first / BITS_PER_BLOCK); b < nblocks; b++) {
        struct buf *buf = bread(m->dev, start + b);
        if (!buf)
            return 0;
        uint64_t base = (uint64_t)b * BITS_PER_BLOCK;
        for (uint64_t bit = base < first ? first - base : 0; bit < BITS_PER_BLOCK; bit++) {
            if (base + bit >= limit)
                break;
            uint8_t *byte = &buf->data[bit / 8];
            if (!(*byte & (1 << (bit % 8)))) {
                *byte |= (uint8_t)(1 << (bit % 8));
                mfs_journal_write(m, buf);
                brelse(buf);
                return base + bit;
            }
        }
        brelse(buf);
    }
    return 0;
}

static void bitmap_clear(struct mfs_sb *m, uint32_t start, uint64_t bit)
{
    kassert(mutex_held(&m->lock));
    struct buf *buf = bread(m->dev, start + (uint32_t)(bit / BITS_PER_BLOCK));
    if (!buf)
        return;
    uint64_t i = bit % BITS_PER_BLOCK;
    kassert(buf->data[i / 8] & (1 << (i % 8)));
    buf->data[i / 8] &= (uint8_t)~(1 << (i % 8));
    mfs_journal_write(m, buf);
    brelse(buf);
}

uint32_t mfs_alloc_block(struct mfs_sb *m)
{
    mutex_lock(&m->lock);
    uint64_t b = bitmap_alloc(m, m->sb.block_bitmap_start, m->sb.block_bitmap_blocks,
                              m->sb.data_start, m->sb.nblocks);
    if (b) {
        m->sb.free_blocks--;
        mfs_super_journal(m);
    }
    mutex_unlock(&m->lock);
    if (!b)
        return 0;
    /* Hand out zeroed blocks. The zeroes are data as far as the journal is
     * concerned: they reach the disk before the transaction commits. */
    struct buf *buf = bread(m->dev, b);
    if (!buf) {
        mfs_free_block(m, (uint32_t)b);
        return 0;
    }
    memset(buf->data, 0, MFS_BLOCK_SIZE);
    bwrite(buf);
    brelse(buf);
    return (uint32_t)b;
}

void mfs_free_block(struct mfs_sb *m, uint32_t block)
{
    kassert(block >= m->sb.data_start && block < m->sb.nblocks);
    mutex_lock(&m->lock);
    bitmap_clear(m, m->sb.block_bitmap_start, block);
    m->sb.free_blocks++;
    mfs_super_journal(m);
    mutex_unlock(&m->lock);
}

uint32_t mfs_alloc_inode(struct mfs_sb *m)
{
    mutex_lock(&m->lock);
    uint64_t i = bitmap_alloc(m, m->sb.inode_bitmap_start, m->sb.inode_bitmap_blocks,
                              1, m->sb.ninodes);
    if (i) {
        m->sb.free_inodes--;
        mfs_super_journal(m);
    }
    mutex_unlock(&m->lock);
    return (uint32_t)i;
}

void mfs_free_inode(struct mfs_sb *m, uint32_t ino)
{
    kassert(ino > 0 && ino < m->sb.ninodes);
    mutex_lock(&m->lock);
    bitmap_clear(m, m->sb.inode_bitmap_start, ino);
    m->sb.free_inodes++;
    mfs_super_journal(m);
    mutex_unlock(&m->lock);
}
