#pragma once
#include <kernel.h>

/* The initial ramdisk is a ustar archive loaded by Limine as a module. It
 * is parsed once at boot into a table with one entry per member, allocated
 * from the heap after the members are counted. The contents are read from
 * the module memory, which is mapped in the direct map. Until the VFS
 * exists (M11) this table is the only filesystem. */
#define INITRD_NAME_MAX    100

enum initrd_type {
    INITRD_FILE,
    INITRD_DIR,
    INITRD_LINK,                      /* symbolic link, typeflag 2 */
};

struct initrd_entry {
    char name[INITRD_NAME_MAX + 1];   /* normalized, no leading slash, no trailing slash */
    enum initrd_type type;
    const uint8_t *data;              /* contents; for a link its target in the header */
    size_t size;
    uint64_t mtime;                   /* seconds since the epoch, from the tar header */
    uint32_t mode;                    /* permission bits of the tar header (U1) */
    uint32_t uid, gid;
};

void initrd_init(void);
/* The newest modification time of any entry (the root directory's). */
uint64_t initrd_newest_mtime(void);
/* Look up a normalized absolute path such as "/bin/init". */
const struct initrd_entry *initrd_lookup(const char *path);
size_t initrd_count(void);
const struct initrd_entry *initrd_entry(size_t index);
