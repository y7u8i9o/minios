#pragma once
#include <kernel.h>
#include <limine.h>

#define BOOT_MAX_MEMMAP 128
#define BOOT_MAX_CMDLINE 256

/* Information gathered from the Limine responses at entry. Everything is
 * copied out of bootloader reclaimable memory so that memory can be freed
 * once the kernel runs on its own page tables. Written once by boot_init
 * before any other subsystem runs, read only afterwards. */
struct bootinfo {
    uint64_t hhdm_offset;
    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
    char cmdline[BOOT_MAX_CMDLINE];
    bool have_framebuffer;
    struct limine_framebuffer framebuffer;
    const uint8_t *initrd;          /* module contents in the direct map */
    uint64_t initrd_size;
    size_t memmap_count;
    struct limine_memmap_entry memmap[BOOT_MAX_MEMMAP];
};

extern struct bootinfo bootinfo;

void boot_init(void);
