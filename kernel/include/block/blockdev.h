#pragma once
#include <kernel.h>
#include <lib/list.h>

#define BLOCKDEV_NAME_LEN 16

/* A block device. Devices are registered once and never removed; the
 * list is protected by blockdev_lock. The driver serializes its own
 * requests, so rw may be called concurrently from several threads. */
struct blockdev {
    char name[BLOCKDEV_NAME_LEN];
    uint32_t sector_size;
    uint64_t nsectors;
    /* Transfer count sectors starting at sector. Blocks until complete. */
    int (*rw)(struct blockdev *dev, uint64_t sector, uint32_t count, void *buf, bool write);
    /* Flush the device write cache, if any. May be NULL. */
    int (*flush)(struct blockdev *dev);
    void *priv;
    struct list_head link;
};

void blockdev_init(void);
/* Register dev and create /dev/<name> backed by the block cache. */
int blockdev_register(struct blockdev *dev);
struct blockdev *blockdev_find(const char *name);
int blockdev_read(struct blockdev *dev, uint64_t sector, uint32_t count, void *buf);
int blockdev_write(struct blockdev *dev, uint64_t sector, uint32_t count, const void *buf);
uint64_t blockdev_size(struct blockdev *dev);
