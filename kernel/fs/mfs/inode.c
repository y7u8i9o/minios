#define KLOG_SUBSYS "mfs"
#include "mfs.h"
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

static struct mfs_sb *mfs_of(struct inode *ino)
{
    return ino->sb->priv;
}

/* Read or set one pointer inside an indirect block. */
static uint32_t ptr_get(struct mfs_sb *m, uint32_t block, uint32_t index)
{
    struct buf *b = bread(m->dev, block);
    if (!b)
        return 0;
    uint32_t v = ((uint32_t *)b->data)[index];
    brelse(b);
    return v;
}

static int ptr_set(struct mfs_sb *m, uint32_t block, uint32_t index, uint32_t v)
{
    struct buf *b = bread(m->dev, block);
    if (!b)
        return -EIO;
    ((uint32_t *)b->data)[index] = v;
    mfs_journal_write(m, b);
    brelse(b);
    return 0;
}

/* Map file block idx to a disk block. With alloc, missing blocks and
 * indirect tables are allocated. Returns 0 for a hole or on failure. */
static uint32_t bmap(struct inode *ino, uint64_t idx, bool alloc)
{
    struct mfs_sb *m = mfs_of(ino);
    struct mfs_inode_info *info = ino->priv;
    if (idx < MFS_NDIRECT) {
        if (!info->direct[idx] && alloc) {
            info->direct[idx] = mfs_alloc_block(m);
            mfs_inode_flush(ino);
        }
        return info->direct[idx];
    }
    idx -= MFS_NDIRECT;
    if (idx < MFS_PTRS_PER_BLOCK) {
        if (!info->indirect) {
            if (!alloc)
                return 0;
            info->indirect = mfs_alloc_block(m);
            if (!info->indirect)
                return 0;
            mfs_inode_flush(ino);
        }
        uint32_t v = ptr_get(m, info->indirect, (uint32_t)idx);
        if (!v && alloc) {
            v = mfs_alloc_block(m);
            if (v)
                ptr_set(m, info->indirect, (uint32_t)idx, v);
        }
        return v;
    }
    idx -= MFS_PTRS_PER_BLOCK;
    if (idx >= (uint64_t)MFS_PTRS_PER_BLOCK * MFS_PTRS_PER_BLOCK)
        return 0;
    if (!info->dindirect) {
        if (!alloc)
            return 0;
        info->dindirect = mfs_alloc_block(m);
        if (!info->dindirect)
            return 0;
        mfs_inode_flush(ino);
    }
    uint32_t i1 = (uint32_t)(idx / MFS_PTRS_PER_BLOCK), i2 = (uint32_t)(idx % MFS_PTRS_PER_BLOCK);
    uint32_t l1 = ptr_get(m, info->dindirect, i1);
    if (!l1) {
        if (!alloc)
            return 0;
        l1 = mfs_alloc_block(m);
        if (!l1)
            return 0;
        ptr_set(m, info->dindirect, i1, l1);
    }
    uint32_t v = ptr_get(m, l1, i2);
    if (!v && alloc) {
        v = mfs_alloc_block(m);
        if (v)
            ptr_set(m, l1, i2, v);
    }
    return v;
}

long mfs_read_locked(struct inode *ino, char *buf, size_t n, uint64_t off)
{
    struct mfs_sb *m = mfs_of(ino);
    if (off >= ino->size)
        return 0;
    if (n > ino->size - off)
        n = ino->size - off;
    size_t done = 0;
    while (done < n) {
        uint64_t idx = off / MFS_BLOCK_SIZE;
        size_t boff = off % MFS_BLOCK_SIZE;
        size_t chunk = MIN(n - done, MFS_BLOCK_SIZE - boff);
        uint32_t blk = bmap(ino, idx, false);
        if (!blk) {
            memset(buf + done, 0, chunk);
        } else {
            struct buf *b = bread(m->dev, blk);
            if (!b)
                return done ? (long)done : -EIO;
            memcpy(buf + done, b->data + boff, chunk);
            brelse(b);
        }
        done += chunk;
        off += chunk;
    }
    return (long)done;
}

/* Directory contents and symbolic link targets are metadata and go through
 * the journal; file data is written back by the cache (before the
 * transaction commits). */
long mfs_write_locked(struct inode *ino, const char *buf, size_t n, uint64_t off)
{
    struct mfs_sb *m = mfs_of(ino);
    bool metadata = S_ISDIR(ino->mode) || S_ISLNK(ino->mode);
    if ((off + n) / MFS_BLOCK_SIZE >= MFS_MAX_FILE_BLOCKS)
        return -EFBIG;
    size_t done = 0;
    bool flushed = false;
    ino->mtime = vfs_now();
    while (done < n) {
        uint64_t idx = off / MFS_BLOCK_SIZE;
        size_t boff = off % MFS_BLOCK_SIZE;
        size_t chunk = MIN(n - done, MFS_BLOCK_SIZE - boff);
        uint32_t blk = bmap(ino, idx, true);
        if (!blk)
            return done ? (long)done : -ENOSPC;
        struct buf *b = bread(m->dev, blk);
        if (!b)
            return done ? (long)done : -EIO;
        memcpy(b->data + boff, buf + done, chunk);
        if (metadata)
            mfs_journal_write(m, b);
        else
            bwrite(b);
        brelse(b);
        done += chunk;
        off += chunk;
        if (off > ino->size) {
            ino->size = off;
            mfs_inode_flush(ino);
            flushed = true;
        }
    }
    if (done && !flushed)
        mfs_inode_flush(ino);           /* the modification time */
    return (long)done;
}

/* Free every block with index >= first. A table that is released as a
 * whole is not rewritten first; only tables that stay have their freed
 * entries cleared (through the journal). */
static void free_from(struct inode *ino, uint64_t first)
{
    struct mfs_sb *m = mfs_of(ino);
    struct mfs_inode_info *info = ino->priv;
    for (uint64_t i = first; i < MFS_NDIRECT; i++) {
        if (info->direct[i]) {
            mfs_free_block(m, info->direct[i]);
            info->direct[i] = 0;
        }
    }
    if (info->indirect) {
        uint64_t base = MFS_NDIRECT;
        bool keep = first > base;
        for (uint32_t i = 0; i < MFS_PTRS_PER_BLOCK; i++) {
            if (base + i < first)
                continue;
            uint32_t v = ptr_get(m, info->indirect, i);
            if (v) {
                mfs_free_block(m, v);
                if (keep)
                    ptr_set(m, info->indirect, i, 0);
            }
        }
        if (!keep) {
            mfs_free_block(m, info->indirect);
            info->indirect = 0;
        }
    }
    if (info->dindirect) {
        uint64_t base = MFS_NDIRECT + MFS_PTRS_PER_BLOCK;
        bool keep_l1 = first > base;
        for (uint32_t i1 = 0; i1 < MFS_PTRS_PER_BLOCK; i1++) {
            uint32_t l1 = ptr_get(m, info->dindirect, i1);
            if (!l1)
                continue;
            uint64_t l1_base = base + (uint64_t)i1 * MFS_PTRS_PER_BLOCK;
            bool keep_l2 = first > l1_base;
            for (uint32_t i2 = 0; i2 < MFS_PTRS_PER_BLOCK; i2++) {
                if (l1_base + i2 < first)
                    continue;
                uint32_t v = ptr_get(m, l1, i2);
                if (v) {
                    mfs_free_block(m, v);
                    if (keep_l2)
                        ptr_set(m, l1, i2, 0);
                }
            }
            if (!keep_l2) {
                mfs_free_block(m, l1);
                if (keep_l1)
                    ptr_set(m, info->dindirect, i1, 0);
            }
        }
        if (!keep_l1) {
            mfs_free_block(m, info->dindirect);
            info->dindirect = 0;
        }
    }
}

int mfs_truncate_locked(struct inode *ino, uint64_t size)
{
    struct mfs_sb *m = mfs_of(ino);
    if (size / MFS_BLOCK_SIZE >= MFS_MAX_FILE_BLOCKS)
        return -EFBIG;
    if (size < ino->size) {
        uint64_t keep = (size + MFS_BLOCK_SIZE - 1) / MFS_BLOCK_SIZE;
        free_from(ino, keep);
        /* Zero the tail of the last kept block so later growth reads zeros. */
        if (size % MFS_BLOCK_SIZE) {
            uint32_t blk = bmap(ino, size / MFS_BLOCK_SIZE, false);
            if (blk) {
                struct buf *b = bread(m->dev, blk);
                if (b) {
                    memset(b->data + size % MFS_BLOCK_SIZE, 0, MFS_BLOCK_SIZE - size % MFS_BLOCK_SIZE);
                    bwrite(b);
                    brelse(b);
                }
            }
        }
    }
    ino->size = size;
    ino->mtime = vfs_now();
    return mfs_inode_flush(ino);
}

void mfs_free_all_blocks(struct inode *ino)
{
    free_from(ino, 0);
    ino->size = 0;
    mfs_inode_flush(ino);
}

/* ---- file operations ---- */

static long mfs_file_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct inode *ino = f->inode;
    mutex_lock(&ino->lock);
    long r = mfs_read_locked(ino, buf, n, *pos);
    mutex_unlock(&ino->lock);
    if (r > 0)
        *pos += (uint64_t)r;
    return r;
}

static long mfs_file_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct inode *ino = f->inode;
    mutex_lock(&ino->lock);
    if (f->flags & O_APPEND)
        *pos = ino->size;
    long r = mfs_write_locked(ino, buf, n, *pos);
    mutex_unlock(&ino->lock);
    if (r > 0)
        *pos += (uint64_t)r;
    return r;
}

const struct file_ops mfs_file_fops = {
    .read = mfs_file_read,
    .write = mfs_file_write,
};
