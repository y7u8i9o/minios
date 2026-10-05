#pragma once
/* Names of block devices (docs/design/block.md). A partition is named
 * after its disk and its GPT index. A disk whose name ends in a digit, such
 * as nvme0n1, has a "p" before the index, as nvme0n1p1. The header is
 * self-contained, which lets the host build of pkg include it. */
#include <string.h>

/* The GPT index of the partition name on the disk disk, as a string, or
 * NULL when name is no partition of disk. */
static inline const char *disk_partition_index(const char *name, const char *disk)
{
    size_t n = strlen(disk);
    if (n == 0 || strncmp(name, disk, n) != 0)
        return NULL;
    name += n;
    if (disk[n - 1] >= '0' && disk[n - 1] <= '9') {
        if (*name != 'p')
            return NULL;
        name++;
    }
    for (const char *c = name; *c; c++)
        if (*c < '0' || *c > '9')
            return NULL;
    return *name ? name : NULL;
}
