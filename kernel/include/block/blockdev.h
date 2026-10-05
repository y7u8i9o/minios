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
    struct blockdev *disk;      /* the disk of a partition (block/part.h), NULL for a disk */
    uint32_t flags;             /* BLOCKDEV_*, set by the driver before registration */
    struct list_head link;
};

/* A drive with removable media of 2048 byte sectors, which the root
 * search of a disk image skips. */
#define BLOCKDEV_CDROM     (1u << 0)
/* A device that refuses writes with EROFS. */
#define BLOCKDEV_READONLY  (1u << 1)

void blockdev_init(void);
/* Register dev and create /dev/<name> backed by the block cache. */
int blockdev_register(struct blockdev *dev);
struct blockdev *blockdev_find(const char *name);
/* The next free name of a kind of device: sda, sdb, ... for disks of the
 * SATA and USB drivers (letters true), sr0, sr1, ... for CD drives
 * (letters false). Each call takes a new name from the sequence of
 * prefix. */
void blockdev_next_name(char *name, size_t size, const char *prefix, bool letters);
/* Fill devs with up to max registered devices in the order of their
 * registration and return the count. Devices are never removed, and the
 * pointers therefore remain valid. */
int blockdev_list(struct blockdev **devs, int max);
int blockdev_read(struct blockdev *dev, uint64_t sector, uint32_t count, void *buf);
int blockdev_write(struct blockdev *dev, uint64_t sector, uint32_t count, const void *buf);
uint64_t blockdev_size(struct blockdev *dev);
