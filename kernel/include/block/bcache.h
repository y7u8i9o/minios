#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sync/mutex.h>

struct blockdev;

#define BCACHE_BLOCK_SIZE 4096
#define BCACHE_NBUF       256

/* A cached block. dev, block, refcount, valid, dirty, pinned and the list
 * links are protected by bcache_lock. data is protected by lock, locked by
 * the owner between bread and brelse. A pinned buffer belongs to a
 * filesystem transaction (M36): it contains an extra reference so it cannot
 * be evicted, and bcache_sync leaves it alone until the filesystem writes
 * it with bwrite_now and unpins it. */
struct buf {
    struct blockdev *dev;
    uint64_t block;
    uint8_t *data;
    int refcount;
    bool valid;
    bool dirty;
    bool pinned;
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
/* Write every dirty, unpinned buffer of dev (NULL for all) to disk and
 * flush the device cache. */
int bcache_sync(struct blockdev *dev);
/* Mark the locked buffer dirty and pinned (a no-op when already pinned).
 * Returns true when the buffer was newly pinned. */
bool bpin(struct buf *b);
/* Drop the pin of a buffer the caller has locked; the buffer remains
 * referenced by the caller until brelse. */
void bunpin(struct buf *b);
/* Write the locked buffer to its block now, clearing dirty. */
int bwrite_now(struct buf *b);
/* Drop the contents of a locked buffer without writing them (dirty and
 * valid are cleared); used to simulate a power loss in tests. */
void bforget(struct buf *b);
/* Forget every unreferenced buffer of dev, written or not. */
void bcache_discard(struct blockdev *dev);
/* Number of dirty buffers, for tests. */
size_t bcache_dirty_count(void);
