#define KLOG_SUBSYS "bcache"
#include <block/bcache.h>
#include <block/blockdev.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* Fixed pool of buffers on one LRU list. bcache_lock protects the list
 * and the identity, reference count and flags of every buffer. */
static struct buf bufs[BCACHE_NBUF];
static LIST_HEAD(lru);
static DEFINE_SPINLOCK(bcache_lock);

void bcache_init(void)
{
    for (size_t i = 0; i < BCACHE_NBUF; i++) {
        bufs[i].data = kmalloc(BCACHE_BLOCK_SIZE);
        kassert(bufs[i].data != NULL);
        mutex_init(&bufs[i].lock, "buf");
        list_add_tail(&bufs[i].lru, &lru);
    }
    klog_info("%u buffers of %u bytes", BCACHE_NBUF, BCACHE_BLOCK_SIZE);
}

static int write_back(struct buf *b)
{
    uint32_t spb = BCACHE_BLOCK_SIZE / b->dev->sector_size;
    int r = blockdev_write(b->dev, b->block * spb, spb, b->data);
    if (r == 0) {
        spin_lock(&bcache_lock);
        b->dirty = false;
        spin_unlock(&bcache_lock);
    }
    return r;
}

/* Find or allocate a buffer for (dev, block) with a reference taken. */
static struct buf *bget(struct blockdev *dev, uint64_t block)
{
    for (;;) {
        spin_lock(&bcache_lock);
        struct list_head *pos;
        list_for_each(pos, &lru) {
            struct buf *b = list_entry(pos, struct buf, lru);
            if (b->dev == dev && b->block == block) {
                b->refcount++;
                list_del(&b->lru);
                list_add(&b->lru, &lru);
                spin_unlock(&bcache_lock);
                return b;
            }
        }
        /* Evict the least recently used clean, unreferenced buffer. */
        struct buf *victim = NULL, *dirty = NULL;
        for (pos = lru.prev; pos != &lru; pos = pos->prev) {
            struct buf *b = list_entry(pos, struct buf, lru);
            if (b->refcount)
                continue;
            if (!b->dirty) {
                victim = b;
                break;
            }
            if (!dirty)
                dirty = b;
        }
        if (victim) {
            victim->dev = dev;
            victim->block = block;
            victim->valid = false;
            victim->refcount = 1;
            list_del(&victim->lru);
            list_add(&victim->lru, &lru);
            spin_unlock(&bcache_lock);
            return victim;
        }
        if (!dirty) {
            spin_unlock(&bcache_lock);
            klog_error("no free buffers");
            return NULL;
        }
        /* Write the oldest dirty buffer back, then retry. */
        dirty->refcount++;
        spin_unlock(&bcache_lock);
        mutex_lock(&dirty->lock);
        if (dirty->dirty)
            write_back(dirty);
        mutex_unlock(&dirty->lock);
        spin_lock(&bcache_lock);
        dirty->refcount--;
        spin_unlock(&bcache_lock);
    }
}

struct buf *bread(struct blockdev *dev, uint64_t block)
{
    struct buf *b = bget(dev, block);
    if (!b)
        return NULL;
    mutex_lock(&b->lock);
    if (!b->valid) {
        uint32_t spb = BCACHE_BLOCK_SIZE / dev->sector_size;
        if (blockdev_read(dev, block * spb, spb, b->data) < 0) {
            mutex_unlock(&b->lock);
            spin_lock(&bcache_lock);
            b->refcount--;
            b->dev = NULL;
            spin_unlock(&bcache_lock);
            return NULL;
        }
        spin_lock(&bcache_lock);
        b->valid = true;
        spin_unlock(&bcache_lock);
    }
    return b;
}

void bwrite(struct buf *b)
{
    kassert(mutex_held(&b->lock));
    spin_lock(&bcache_lock);
    b->dirty = true;
    spin_unlock(&bcache_lock);
}

void brelse(struct buf *b)
{
    kassert(mutex_held(&b->lock));
    mutex_unlock(&b->lock);
    spin_lock(&bcache_lock);
    kassert(b->refcount > 0);
    b->refcount--;
    spin_unlock(&bcache_lock);
}

int bcache_sync(struct blockdev *dev)
{
    int r = 0;
    struct blockdev *last = NULL;
    for (size_t i = 0; i < BCACHE_NBUF; i++) {
        struct buf *b = &bufs[i];
        spin_lock(&bcache_lock);
        bool want = b->dirty && !b->pinned && b->dev && (!dev || b->dev == dev);
        if (want)
            b->refcount++;
        spin_unlock(&bcache_lock);
        if (!want)
            continue;
        mutex_lock(&b->lock);
        if (b->dirty) {
            int e = write_back(b);
            if (e < 0 && r == 0)
                r = e;
        }
        last = b->dev;
        mutex_unlock(&b->lock);
        spin_lock(&bcache_lock);
        b->refcount--;
        spin_unlock(&bcache_lock);
    }
    if (last && last->flush)
        last->flush(last);
    else if (dev && dev->flush)
        dev->flush(dev);
    return r;
}

bool bpin(struct buf *b)
{
    kassert(mutex_held(&b->lock));
    spin_lock(&bcache_lock);
    b->dirty = true;
    bool fresh = !b->pinned;
    if (fresh) {
        b->pinned = true;
        b->refcount++;
    }
    spin_unlock(&bcache_lock);
    return fresh;
}

void bunpin(struct buf *b)
{
    kassert(mutex_held(&b->lock));
    spin_lock(&bcache_lock);
    kassert(b->pinned && b->refcount > 0);
    b->pinned = false;
    b->refcount--;
    spin_unlock(&bcache_lock);
}

int bwrite_now(struct buf *b)
{
    kassert(mutex_held(&b->lock));
    return write_back(b);
}

void bforget(struct buf *b)
{
    kassert(mutex_held(&b->lock));
    spin_lock(&bcache_lock);
    b->dirty = false;
    b->valid = false;
    spin_unlock(&bcache_lock);
}

void bcache_discard(struct blockdev *dev)
{
    spin_lock(&bcache_lock);
    for (size_t i = 0; i < BCACHE_NBUF; i++) {
        struct buf *b = &bufs[i];
        if (b->dev != dev)
            continue;
        if (b->refcount) {
            klog_warn("block %lu of %s is referenced, not discarded", b->block, dev->name);
            continue;
        }
        b->dirty = false;
        b->valid = false;
        b->dev = NULL;
    }
    spin_unlock(&bcache_lock);
}

size_t bcache_dirty_count(void)
{
    size_t n = 0;
    spin_lock(&bcache_lock);
    for (size_t i = 0; i < BCACHE_NBUF; i++)
        if (bufs[i].dirty)
            n++;
    spin_unlock(&bcache_lock);
    return n;
}
