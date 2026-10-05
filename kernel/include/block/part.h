#pragma once
/* GPT partitions (kernel/block/part.c, docs/design/block.md). Each
 * partition of a disk with a valid GUID partition table is a block device
 * of its own, named after the disk and its index from 1, as vda1, with a p
 * between them when the name of the disk ends in a digit, as nvme0n1p1. */
#include <kernel.h>
#include <block/blockdev.h>


/* A partition. A partition is registered once for an entry number of a
 * disk and never removed. part_rescan changes first, bdev.nsectors, type
 * and uuid, and a partition that disappeared from the table has no
 * sectors. These fields, size_reported and the list are protected by
 * part_lock, and index and bdev.name do not change. */
struct partition {
    struct blockdev bdev;           /* the device, bdev.disk is the disk */
    uint64_t first;                 /* first sector on the disk */
    int index;                      /* the entry of the table, from 1 */
    uint8_t type[16], uuid[16];     /* GUIDs in their on-disk byte order */
    uint64_t size_reported;         /* the size last given to devfs */
    struct list_head link;
};

/* The partition type GUIDs of the Discoverable Partitions Specification
 * that the kernel uses: the root partition of the machine and swap. */
extern const uint8_t part_type_root[16], part_type_swap[16];

/* Read the partition table of every registered disk and register its
 * partitions. Called once, from a thread, since it reads the disks. */
void part_scan(void);
/* Read the table of a disk registered after part_scan, such as a USB disk
 * connected later. Called by blockdev_register; a no-op before the scan,
 * for a partition and for a CD drive. */
void part_add_disk(struct blockdev *disk);
/* Read the table of disk again, after a program wrote it (BLKRRPART).
 * Returns -EBUSY when the root or swap lies on the disk. */
int part_rescan(struct blockdev *disk);
/* Mark dev, a disk or a partition, as in use by the root or swap, which
 * makes part_rescan of its disk fail. */
void part_retain(struct blockdev *dev);
/* The disk with a GPT whose disk GUID is the one the bootloader loaded
 * the kernel from, or NULL. */
struct blockdev *part_boot_disk(void);
/* True if disk has a valid GUID partition table. */
bool part_has_table(struct blockdev *disk);
/* The first partition of disk with the type GUID type, or NULL. */
struct partition *part_find_type(struct blockdev *disk, const uint8_t type[16]);
/* The partition with the unique GUID uuid, or NULL. */
struct partition *part_find_uuid(const uint8_t uuid[16]);
