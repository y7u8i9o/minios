#pragma once
#include <kernel.h>

/* The initial ramdisk is a ustar archive loaded by Limine as a module. It
 * is parsed once at boot into a static table; contents stay in the module
 * memory, which is mapped in the direct map. Until the VFS exists (M11)
 * this table is the only filesystem. */
#define INITRD_MAX_ENTRIES 256
#define INITRD_NAME_MAX    100

enum initrd_type {
    INITRD_FILE,
    INITRD_DIR,
};

struct initrd_entry {
    char name[INITRD_NAME_MAX + 1];   /* normalized, no leading slash, no trailing slash */
    enum initrd_type type;
    const uint8_t *data;
    size_t size;
};

void initrd_init(void);
/* Look up a normalized absolute path such as "/bin/init". */
const struct initrd_entry *initrd_lookup(const char *path);
size_t initrd_count(void);
const struct initrd_entry *initrd_entry(size_t index);
