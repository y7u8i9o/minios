#pragma once
/* GPT partitions (kernel/block/part.c, docs/design/block.md). Each
 * partition of a disk with a valid GUID partition table is a block device
 * of its own, named after the disk and its index from 1, as vda1. */
#include <kernel.h>
#include <block/blockdev.h>

#define PART_GUID_STR 37            /* 36 characters and the NUL */

/* A partition. Partitions are registered once, by part_scan, and never
 * removed. The list is protected by part_lock. The other fields do not
 * change after registration. */
struct partition {
    struct blockdev bdev;           /* the device, bdev.disk is the disk */
    uint64_t first;                 /* first sector on the disk */
    int index;                      /* the entry of the table, from 1 */
    uint8_t type[16], uuid[16];     /* GUIDs in their on-disk byte order */
    struct list_head link;
};

/* The partition type GUIDs of the Discoverable Partitions Specification
 * that the kernel uses: the root partition of the machine and swap. */
extern const uint8_t part_type_root[16], part_type_swap[16];

/* Read the partition table of every registered disk and register its
 * partitions. Called once, from a thread, since it reads the disks. */
void part_scan(void);
/* The disk with a GPT whose disk GUID is the one the bootloader loaded
 * the kernel from, or NULL. */
struct blockdev *part_boot_disk(void);
/* True if disk has a valid GUID partition table. */
bool part_has_table(struct blockdev *disk);
/* The first partition of disk with the type GUID type, or NULL. */
struct partition *part_find_type(struct blockdev *disk, const uint8_t type[16]);
/* The partition with the unique GUID uuid, or NULL. */
struct partition *part_find_uuid(const uint8_t uuid[16]);
/* Parse the standard string form of a GUID into the on-disk byte order.
 * Returns 0 or -EINVAL. */
int part_parse_guid(const char *s, uint8_t out[16]);
/* Format a GUID in on-disk byte order as a lowercase string. */
void part_format_guid(const uint8_t guid[16], char out[PART_GUID_STR]);
