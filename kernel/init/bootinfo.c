#define KLOG_SUBSYS "boot"
#include <boot.h>
#include <limine.h>
#include <klog.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <debug/panic.h>
#include <mm/memlayout.h>

/* The Limine boot protocol, which every architecture of minios boots
 * through: the requests, and their responses copied into struct bootinfo
 * (include/boot.h). */

__used __section(".limine_requests_start")
static volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__used __section(".limine_requests")
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__used __section(".limine_requests")
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0, .response = NULL,
};

__used __section(".limine_requests")
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0, .response = NULL,
};

__used __section(".limine_requests")
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0, .response = NULL,
};

__used __section(".limine_requests")
static volatile struct limine_executable_address_request kaddr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0, .response = NULL,
};

__used __section(".limine_requests")
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0, .response = NULL,
};

__used __section(".limine_requests")
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0, .response = NULL,
};

__used __section(".limine_requests")
static volatile struct limine_bootloader_info_request bootloader_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0, .response = NULL,
};

__used __section(".limine_requests_end")
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

struct bootinfo bootinfo;
uintptr_t hhdm_offset;
extern char __kernel_start[], __kernel_end[];

static void boot_parse_video(void);

void boot_init(void)
{
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        panic("limine base revision not supported by the bootloader");
    if (!hhdm_request.response || !memmap_request.response || !kaddr_request.response)
        panic("missing limine responses (hhdm, memmap or executable address)");

    bootinfo.hhdm_offset = hhdm_request.response->offset;
    bootinfo.kernel_phys_base = kaddr_request.response->physical_base;
    bootinfo.kernel_virt_base = kaddr_request.response->virtual_base;
    struct limine_memmap_response *mm = memmap_request.response;
    if (mm->entry_count > BOOT_MAX_MEMMAP)
        panic("memory map has %lu entries, limit is %d", mm->entry_count, BOOT_MAX_MEMMAP);
    for (uint64_t i = 0; i < mm->entry_count; i++)
        bootinfo.memmap[i] = *mm->entries[i];
    bootinfo.memmap_count = mm->entry_count;

    if (cmdline_request.response && cmdline_request.response->cmdline)
        strlcpy(bootinfo.cmdline, cmdline_request.response->cmdline, sizeof bootinfo.cmdline);
    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0) {
        bootinfo.framebuffer = *framebuffer_request.response->framebuffers[0];
        bootinfo.have_framebuffer = true;
    }
    if (module_request.response && module_request.response->module_count > 0) {
        struct limine_file *f = module_request.response->modules[0];
        bootinfo.initrd = f->address;
        bootinfo.initrd_size = f->size;
    }
    hhdm_offset = bootinfo.hhdm_offset;
    cmdline_init(bootinfo.cmdline);
    boot_parse_video();
}

/* video=WxH[xBPP][@SCALE]: the mode itself is applied by Limine (the image
 * builder copies it into limine.conf); the @SCALE suffix asks for an
 * integer UI scale on high density displays, used by the framebuffer
 * console and reported through /dev/fb0 to the window server. */
static void boot_parse_video(void)
{
    char val[32];
    bootinfo.fb_scale = 1;
    if (!cmdline_lookup("video", val, sizeof val))
        return;
    const char *at = strchr(val, '@');
    if (at && at[1] >= '1' && at[1] <= '4' && at[2] == '\0')
        bootinfo.fb_scale = (uint32_t)(at[1] - '0');
    /* WxH[xBPP]: Limine may not offer the mode; the GPU driver can. */
    uint32_t w = 0, h = 0;
    const char *p = val;
    while (*p >= '0' && *p <= '9')
        w = w * 10 + (uint32_t)(*p++ - '0');
    if (*p == 'x') {
        p++;
        while (*p >= '0' && *p <= '9')
            h = h * 10 + (uint32_t)(*p++ - '0');
    }
    if (w >= 320 && h >= 200 && w <= 8192 && h <= 8192) {
        bootinfo.fb_req_width = w;
        bootinfo.fb_req_height = h;
    }
}

/* The bootloader, the kernel image and everything Limine handed over. */
void boot_log_environment(void)
{
    const struct limine_bootloader_info_response *bl = bootloader_request.response;
    klog_info("booted by %s %s, %zu memory map entries", bl ? bl->name : "unknown bootloader",
              bl ? bl->version : "", bootinfo.memmap_count);
    klog_info("kernel image %lu KiB at phys %lx virt %lx, hhdm at %lx",
              (unsigned long)(__kernel_end - __kernel_start) >> 10, bootinfo.kernel_phys_base,
              bootinfo.kernel_virt_base, bootinfo.hhdm_offset);
    if (bootinfo.initrd)
        klog_info("initrd %lu KiB at phys %lx", bootinfo.initrd_size >> 10,
                  (uintptr_t)bootinfo.initrd - bootinfo.hhdm_offset);
    else
        klog_warn("no initrd module");
    if (!bootinfo.have_framebuffer)
        klog_warn("no framebuffer, serial console only");
    klog_info("cmdline \"%s\", log level %d", bootinfo.cmdline, klog_runtime_level);
}
