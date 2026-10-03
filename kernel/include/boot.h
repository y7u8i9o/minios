#pragma once
#include <kernel.h>
#include <limine.h>

#define BOOT_MAX_MEMMAP 128
#define BOOT_MAX_CMDLINE 256

/* The Limine boot protocol is used on every architecture, so this
 * structure and its Limine types are generic (init/bootinfo.c).
 *
 * Information gathered from the Limine responses at entry. Everything is
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
    uint32_t fb_scale;              /* integer UI scale from video=WxH@N, 1 by default */
    uint32_t fb_req_width, fb_req_height;   /* the video= size or 0, applied by a GPU driver */
    const uint8_t *initrd;          /* module contents in the direct map */
    /* The GPT disk and partition GUIDs of the disk the kernel was loaded
     * from, in their on-disk byte order, zero when it has no GPT. */
    uint8_t boot_disk_guid[16], boot_part_guid[16];
    const void *dtb;                /* the device tree or NULL, valid until pmm_reclaim_bootloader */
    uint64_t initrd_size;
    size_t memmap_count;
    struct limine_memmap_entry memmap[BOOT_MAX_MEMMAP];
};

extern struct bootinfo bootinfo;

void boot_init(void);
/* Log the bootloader, the kernel image and what Limine handed over. */
void boot_log_environment(void);
