#define KLOG_SUBSYS "block"
#include <debug/profile.h>
#include <drivers/timer.h>
#include <block/blockdev.h>
#include <block/bcache.h>
#include <block/part.h>
#include <sched/cred.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <klog.h>
#include <errno.h>

/* Registered devices and the name sequences of blockdev_next_name.
 * Protected by blockdev_lock. */
static LIST_HEAD(blockdevs);
static DEFINE_SPINLOCK(blockdev_lock);
static struct {
    char prefix[4];
    unsigned next;
} name_seqs[4];

void blockdev_init(void)
{
    bcache_init();
}

/* /dev/<name>: byte addressed access through the block cache. */
static long bdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct blockdev *dev = f->inode->priv;
    uint64_t size = blockdev_size(dev);
    if (*pos >= size)
        return 0;
    if (n > size - *pos)
        n = size - *pos;
    size_t done = 0;
    while (done < n) {
        uint64_t block = *pos / BCACHE_BLOCK_SIZE;
        size_t off = *pos % BCACHE_BLOCK_SIZE;
        size_t chunk = MIN(n - done, BCACHE_BLOCK_SIZE - off);
        struct buf *b = bread(dev, block);
        if (!b)
            return done ? (long)done : -EIO;
        memcpy(buf + done, b->data + off, chunk);
        brelse(b);
        done += chunk;
        *pos += chunk;
    }
    return (long)done;
}

static long bdev_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct blockdev *dev = f->inode->priv;
    uint64_t size = blockdev_size(dev);
    if (*pos >= size)
        return -ENOSPC;
    if (n > size - *pos)
        n = size - *pos;
    size_t done = 0;
    while (done < n) {
        uint64_t block = *pos / BCACHE_BLOCK_SIZE;
        size_t off = *pos % BCACHE_BLOCK_SIZE;
        size_t chunk = MIN(n - done, BCACHE_BLOCK_SIZE - off);
        struct buf *b = bread(dev, block);
        if (!b)
            return done ? (long)done : -EIO;
        memcpy(b->data + off, buf + done, chunk);
        bwrite(b);
        brelse(b);
        done += chunk;
        *pos += chunk;
    }
    return (long)done;
}

/* BLKRRPART reads the partition table of a disk again (block/part.h).
 * Only root may ask, since a disk is written through it. */
static long bdev_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    struct blockdev *dev = f->inode->priv;
    if (req != BLKRRPART)
        return -ENOTTY;
    if (dev->disk)
        return -EINVAL;
    if (!cred_current_is_root())
        return -EPERM;
    bcache_sync(dev);
    return part_rescan(dev);
}

static const struct file_ops bdev_fops = {
    .read = bdev_read,
    .write = bdev_write,
    .ioctl = bdev_ioctl,
};

int blockdev_register(struct blockdev *dev)
{
    if (blockdev_find(dev->name))
        return -EEXIST;
    spin_lock(&blockdev_lock);
    list_add_tail(&dev->link, &blockdevs);
    spin_unlock(&blockdev_lock);
    int r = devfs_register(dev->name, S_IFBLK | 0600, &bdev_fops, dev, blockdev_size(dev));
    if (r == 0)
        part_add_disk(dev);
    return r;
}

struct blockdev *blockdev_find(const char *name)
{
    struct list_head *pos;
    struct blockdev *found = NULL;
    spin_lock(&blockdev_lock);
    list_for_each(pos, &blockdevs) {
        struct blockdev *d = list_entry(pos, struct blockdev, link);
        if (strcmp(d->name, name) == 0) {
            found = d;
            break;
        }
    }
    spin_unlock(&blockdev_lock);
    return found;
}

void blockdev_next_name(char *name, size_t size, const char *prefix, bool letters)
{
    unsigned n = 0;
    spin_lock(&blockdev_lock);
    for (size_t i = 0; i < ARRAY_SIZE(name_seqs); i++) {
        if (!name_seqs[i].prefix[0])
            strlcpy(name_seqs[i].prefix, prefix, sizeof name_seqs[i].prefix);
        if (strcmp(name_seqs[i].prefix, prefix) == 0) {
            n = name_seqs[i].next++;
            break;
        }
    }
    spin_unlock(&blockdev_lock);
    if (!letters)
        ksnprintf(name, size, "%s%u", prefix, n);
    else if (n < 26)
        ksnprintf(name, size, "%s%c", prefix, 'a' + n);
    else
        ksnprintf(name, size, "%s%c%c", prefix, 'a' + n / 26 - 1, 'a' + n % 26);
}

int blockdev_list(struct blockdev **devs, int max)
{
    struct list_head *pos;
    int n = 0;
    spin_lock(&blockdev_lock);
    list_for_each(pos, &blockdevs) {
        if (n == max)
            break;
        devs[n++] = list_entry(pos, struct blockdev, link);
    }
    spin_unlock(&blockdev_lock);
    return n;
}

/* Both transfer entry points time themselves for the profiler, which
 * charges the latency and the bytes to the stack that asked for them. */
int blockdev_read(struct blockdev *dev, uint64_t sector, uint32_t count, void *buf)
{
    bool timed = profile_wants(PROF_EV_IO);
    uint64_t start = timed ? timer_ns() : 0;
    int r = dev->rw(dev, sector, count, buf, false);
    if (timed)
        profile_io(false, true, (uint64_t)count * dev->sector_size, start);
    return r;
}

int blockdev_write(struct blockdev *dev, uint64_t sector, uint32_t count, const void *buf)
{
    bool timed = profile_wants(PROF_EV_IO);
    uint64_t start = timed ? timer_ns() : 0;
    int r = dev->rw(dev, sector, count, (void *)buf, true);
    if (timed)
        profile_io(true, true, (uint64_t)count * dev->sector_size, start);
    return r;
}

uint64_t blockdev_size(struct blockdev *dev)
{
    return dev->nsectors * dev->sector_size;
}
