#define KLOG_SUBSYS "boot"
#include <kernel.h>
#include <limine.h>
#include <arch/boot.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/trap.h>
#include <console.h>
#include <klog.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <drivers/serial.h>
#include <drivers/fbcon.h>
#include <debug/symbols.h>
#include <debug/profile.h>
#include <sync/spinlock.h>
#include <debug/panic.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/slab.h>
#include <fs/initrd.h>
#include <fs/vfs.h>
#include <fs/initrdfs.h>
#include <fs/fat.h>
#include <fs/devfs.h>
#include <arch/apic.h>
#include <arch/smp.h>
#include <arch/irq.h>
#include <mm/tlb.h>
#include <drivers/timer.h>
#include <drivers/rtc.h>
#include <ipc/futex.h>
#include <drivers/ps2kbd.h>
#include <drivers/pci.h>
#include <drivers/ps2mouse.h>
#include <drivers/mouse.h>
#include <drivers/virtio/virtio_gpu.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/fbdev.h>
#include <drivers/pty.h>
#include <drivers/virtio/virtio_blk.h>
#include <drivers/virtio/virtio_snd.h>
#include <block/blockdev.h>
#include <mm/swap.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <sync/rcu.h>
#include <sched/proc.h>
#include <sched/user.h>
#include <arch/syscall.h>
#include <tests/ktest.h>
#include <errno.h>

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

__used __section(".limine_requests_end")
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

struct bootinfo bootinfo;
uintptr_t hhdm_offset;

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

static void boot_apply_cmdline(void)
{
    char val[16];
    if (cmdline_lookup("loglevel", val, sizeof val) && val[0] >= '0' && val[0] <= '3')
        klog_set_level(val[0] - '0');
}

/* First thread. Runs the selected self test, then starts init from the
 * initrd and reaps it, which is fatal. */
/* Mount the root: the mfs on vda when it carries a filesystem, otherwise
 * the initrd. root=initrd on the command line forces the initrd. */
static void mount_root(void)
{
    char root[32];
    bool force_initrd = cmdline_lookup("root", root, sizeof root) && strcmp(root, "initrd") == 0;
    int r = -ENODEV;
    if (!force_initrd)
        r = vfs_mount("mfs", "vda", "/");
    if (r < 0) {
        r = vfs_mount("initrd", "initrd", "/");
        if (r < 0)
            panic("cannot mount root: %d", r);
    } else if (vfs_mount("initrd", "initrd", "/initrd") < 0) {
        klog_warn("initrd not mounted on /initrd");
    }
    r = vfs_mount("devfs", "devfs", "/dev");
    if (r < 0)
        panic("cannot mount /dev: %d", r);
}

static void kinit(void *arg)
{
    rcu_start_worker();
    mount_root();
    /* virtio-snd discovery sends synchronous control messages and therefore
     * requires interrupt delivery and a schedulable current thread. */
    virtio_snd_init();
    virtio_gpu_init();
    virtio_input_init();
    swap_start_daemon();
    ps2kbd_start_ttyd();
#if CONFIG_TESTS
    ktest_run_selected();
#endif
    static char init_path[128] = "/bin/init";
    char chosen[128];
    if (cmdline_lookup("init", chosen, sizeof chosen) && chosen[0])
        strlcpy(init_path, chosen, sizeof init_path);
    char *const argv[] = { init_path, NULL };
    char *const envp[] = { "PATH=/bin", NULL };
    struct proc *p = proc_create_user(init_path, argv, envp, &kernel_proc);
    if (!p)
        panic("cannot start %s", init_path);
    proc_set_init(p);
    klog_info("boot complete");
    int status = proc_reap(p);
    panic("init exited with status 0x%x", status);
}

__noreturn void kmain(void)
{
    serial_init();
    console_init();
    boot_init();
    fb_screen_init();
    fbcon_init();
    kprintf("minios booting\n");

    boot_apply_cmdline();
    gdt_init();
    cpu_init_boot();
    klog_ring_init();
    idt_init();
    ksyms_init();

    klog_info("kernel at phys %lx virt %lx, hhdm offset %lx",
              bootinfo.kernel_phys_base, bootinfo.kernel_virt_base, bootinfo.hhdm_offset);
    klog_info("cmdline: \"%s\"", bootinfo.cmdline);

    pmm_init();
    vmm_init();
    /* The APs are parked in bootloader reclaimable memory; move them onto
     * kernel stacks and page tables before that memory is freed. */
    smp_park_aps();
    pmm_reclaim_bootloader();
    slab_init();
    initrd_init();
    vfs_init();
    initrdfs_init();
    devfs_init();
    mfs_init();
    fat_init();
    lapic_init();
    ioapic_init();
    tlb_init();
    timer_init();
    rtc_init();
    ps2kbd_init();
    mouse_init();
    ps2mouse_init();
    fbdev_init();
    pty_init();
    pci_init();
    blockdev_init();
    virtio_blk_init();
    swap_init();
    profile_init();
    lockstat_init();
    syscall_init();
    proc_init();
    futex_init();
    sched_init();
    sti();
    smp_start_aps();

    console_start_daemon();

    if (!thread_create("kinit", kinit, NULL, 0))
        panic("cannot create kinit thread");
    sched_idle_loop();
}
