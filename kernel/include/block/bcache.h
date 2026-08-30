#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sync/mutex.h>

struct blockdev;

#define BCACHE_BLOCK_SIZE 4096
#define BCACHE_NBUF       256

/* A cached block. dev, block, refcount, valid, dirty and the list links
 * are protected by bcache_lock. data is protected by lock, held by the
 * owner between bread and brelse. */
struct buf {
    struct blockdev *dev;
    uint64_t block;
    uint8_t *data;
    int refcount;
    bool valid;
    bool dirty;
    struct mutex lock;
    struct list_head lru;           /* most recently used first */
};

void bcache_init(void);
/* Return block of dev locked and filled. NULL on I/O or allocation failure. */
struct buf *bread(struct blockdev *dev, uint64_t block);
/* Mark the locked buffer dirty; it is written back by bsync or eviction. */
void bwrite(struct buf *b);
/* Unlock and release the buffer. */
void brelse(struct buf *b);
/* Write every dirty buffer of dev (NULL for all) to disk. */
int bcache_sync(struct blockdev *dev);
/* Number of dirty buffers, for tests. */
size_t bcache_dirty_count(void);
