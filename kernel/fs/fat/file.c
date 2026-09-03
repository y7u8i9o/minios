#define KLOG_SUBSYS "fat"
/* File contents: reads and writes cluster by cluster, truncation. The
 * callers hold ino->lock. */
#include "fat.h"
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define FAT_FILE_MAX 0xffffffffu

static long fat_read_locked(struct inode *ino, char *buf, size_t n, uint64_t off)
{
    struct fat_sb *m = fat_of(ino);
    if (off >= ino->size)
        return 0;
    if (n > ino->size - off)
        n = ino->size - off;
    size_t done = 0;
    while (done < n) {
        uint32_t idx = (uint32_t)(off / m->cluster_bytes);
        uint32_t coff = (uint32_t)(off % m->cluster_bytes);
        size_t chunk = MIN(n - done, (size_t)(m->cluster_bytes - coff));
        uint32_t c = fat_cluster_at(ino, idx, false);
        if (!c)
            return done ? (long)done : -EIO;
        int r = fat_read(m, fat_cluster_off(m, c) + coff, buf + done, chunk);
        if (r < 0)
            return done ? (long)done : r;
        done += chunk;
        off += chunk;
    }
    return (long)done;
}

static long fat_write_locked(struct inode *ino, const char *buf, size_t n, uint64_t off)
{
    struct fat_sb *m = fat_of(ino);
    if (off >= FAT_FILE_MAX || n > FAT_FILE_MAX - off)
        return -EFBIG;
    /* No holes: clusters between the old end and off are allocated
     * (zeroed by the allocator). */
    size_t done = 0;
    while (done < n) {
        uint32_t idx = (uint32_t)(off / m->cluster_bytes);
        uint32_t coff = (uint32_t)(off % m->cluster_bytes);
        size_t chunk = MIN(n - done, (size_t)(m->cluster_bytes - coff));
        uint32_t c = fat_cluster_at(ino, idx, true);
        if (!c)
            return done ? (long)done : -ENOSPC;
        int r = fat_write(m, fat_cluster_off(m, c) + coff, buf + done, chunk);
        if (r < 0)
            return done ? (long)done : r;
        done += chunk;
        off += chunk;
        if (off > ino->size)
            ino->size = off;
    }
    fat_inode_flush(ino);
    return (long)done;
}

int fat_truncate_locked(struct inode *ino, uint64_t size)
{
    struct fat_sb *m = fat_of(ino);
    struct fat_inode_info *info = ino->priv;
    if (size > FAT_FILE_MAX)
        return -EFBIG;
    if (size < ino->size) {
        if (size == 0) {
            uint32_t first = info->first_cluster;
            info->first_cluster = 0;
            info->walk_cluster = 0;
            if (first)
                fat_free_chain(m, first);
        } else {
            uint32_t last = (uint32_t)((size - 1) / m->cluster_bytes);
            uint32_t c = fat_cluster_at(ino, last, false);
            if (c) {
                uint32_t next = fat_get(m, c);
                fat_set(m, c, fat_eoc(m));
                if (!fat_is_eoc(m, next) && next >= 2)
                    fat_free_chain(m, next);
                /* Zero the tail of the last cluster so later growth
                 * reads zeros. */
                uint32_t keep = (uint32_t)(size % m->cluster_bytes);
                if (keep) {
                    static const uint8_t zeros[FAT_SECTOR_SIZE];
                    uint64_t off = fat_cluster_off(m, c) + keep;
                    uint32_t left = m->cluster_bytes - keep;
                    while (left) {
                        uint32_t chunk = MIN(left, (uint32_t)FAT_SECTOR_SIZE);
                        fat_write(m, off, zeros, chunk);
                        off += chunk;
                        left -= chunk;
                    }
                }
            }
        }
    } else if (size > ino->size) {
        uint32_t last = (uint32_t)((size - 1) / m->cluster_bytes);
        if (!fat_cluster_at(ino, last, true))
            return -ENOSPC;
    }
    ino->size = size;
    return fat_inode_flush(ino);
}

static long fat_file_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct inode *ino = f->inode;
    mutex_lock(&ino->lock);
    long r = fat_read_locked(ino, buf, n, *pos);
    mutex_unlock(&ino->lock);
    if (r > 0)
        *pos += (uint64_t)r;
    return r;
}

static long fat_file_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct inode *ino = f->inode;
    mutex_lock(&ino->lock);
    if (f->flags & O_APPEND)
        *pos = ino->size;
    long r = fat_write_locked(ino, buf, n, *pos);
    mutex_unlock(&ino->lock);
    if (r > 0)
        *pos += (uint64_t)r;
    return r;
}

const struct file_ops fat_file_fops = {
    .read = fat_file_read,
    .write = fat_file_write,
};
