#define KLOG_SUBSYS "boot"
#include <debug/hung.h>
#include <kernel.h>
#include <boot.h>
#include <arch/init.h>
#include <arch/cpu.h>
#include <arch/smp.h>
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
#include <block/part.h>
#include <lib/guid.h>
#include <fs/fat.h>
#include <fs/tmpfs.h>
#include <fs/devfs.h>
#include <mm/tlb.h>
#include <drivers/timer.h>
#include <drivers/rtc.h>
#include <ipc/futex.h>
#include <ipc/socket.h>
#include <net/net.h>
#include <drivers/pci.h>
#include <input/input.h>
#include <drivers/tty.h>
#include <drivers/virtio/virtio_gpu.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/usb.h>
#include <drivers/nvme.h>
#include <drivers/ahci.h>
#include <drivers/devinfo.h>
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
#include <arch/platform.h>
#include <tests/ktest.h>
#include <errno.h>

/* The kernel start-up sequence, entered from the architecture's entry code
 * on the boot stack. The arch_init_* steps are described in
 * docs/design/arch.md. */

static void boot_apply_cmdline(void)
{
    char val[16];
    if (cmdline_lookup("loglevel", val, sizeof val) && val[0] >= '0' && val[0] <= '3')
        klog_set_level(val[0] - '0');
}

/* The first registered disk that is not a partition and not a CD drive,
 * or NULL. */
static struct blockdev *first_disk(void)
{
    struct blockdev *devs[32];
    int n = blockdev_list(devs, 32);
    for (int i = 0; i < n; i++)
        if (!devs[i]->disk && !(devs[i]->flags & BLOCKDEV_CDROM))
            return devs[i];
    return NULL;
}

/* Mount the root (docs/design/block.md). root=initrd selects the initrd,
 * root=PARTUUID=GUID the partition with that unique GUID and root=NAME the
 * device NAME. Without root=, the kernel mounts the root partition of the
 * Discoverable Partitions Specification on the disk it was loaded from,
 * else on vda, else on the first disk that is not a CD drive, and such a
 * disk without a partition table as a whole. The initrd is the root when
 * nothing else mounts. */
static void mount_root(void)
{
    char root[64], source[BLOCKDEV_NAME_LEN] = "";
    bool given = cmdline_lookup("root", root, sizeof root);
    bool force_initrd = given && strcmp(root, "initrd") == 0;
    if (given && !force_initrd) {
        uint8_t uuid[16];
        struct partition *p = NULL;
        if (strncmp(root, "PARTUUID=", 9) != 0)
            strlcpy(source, root, sizeof source);
        else if (guid_parse(root + 9, uuid) == 0 && (p = part_find_uuid(uuid)))
            strlcpy(source, p->bdev.name, sizeof source);
        else
            klog_error("root=%s: no such partition", root);
    } else if (!force_initrd) {
        struct blockdev *disk = part_boot_disk();
        if (!disk)
            disk = blockdev_find("vda");
        if (!disk)
            disk = first_disk();
        struct partition *p = disk ? part_find_type(disk, part_type_root) : NULL;
        if (p)
            strlcpy(source, p->bdev.name, sizeof source);
        else if (disk && !part_has_table(disk))
            strlcpy(source, disk->name, sizeof source);
    }
    int r = source[0] ? vfs_mount("mfs", source, "/", NULL) : -ENODEV;
    if (r == 0) {
        klog_info("root: mfs on %s", source);
        part_retain(blockdev_find(source));
    }
    if (r < 0) {
        r = vfs_mount("initrd", "initrd", "/", NULL);
        if (r < 0)
            panic("cannot mount root: %d", r);
    } else if (vfs_mount("initrd", "initrd", "/initrd", NULL) < 0) {
        klog_warn("initrd not mounted on /initrd");
    }
    r = vfs_mount("devfs", "devfs", "/dev", NULL);
    if (r < 0)
        panic("cannot mount /dev: %d", r);
}

/* First thread. Runs the selected self test, then starts init from the
 * initrd and reaps it, which is fatal. */
static void kinit(void *arg)
{
    rcu_start_worker();
    /* The NVMe and AHCI probes wait for the devices and therefore run in
     * a thread. */
    nvme_init();
    ahci_init();
    /* The xHCI threads enumerate the USB devices that are connected at
     * boot: disks before the partition scan, input devices before init
     * starts the display server. */
    usb_init();
    /* Reading the partition tables needs a thread, which the block
     * drivers sleep in. */
    part_scan();
    swap_attach();
    mount_root();
    /* virtio-snd discovery sends synchronous control messages and therefore
     * requires interrupt delivery and a schedulable current thread. */
    virtio_snd_init();
    virtio_gpu_init();
    virtio_input_init();
    swap_start_daemon();
    hung_start_daemon();
    tty_start_daemon();
    input_start_daemon();
    /* The network core needs the worker thread before any interface is
     * published; devices register with it in later milestones. */
    net_init();
    devfs_log_nodes();
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
    struct pmm_stats mem;
    pmm_get_stats(&mem);
    klog_info("boot complete in %lu ms, init is %s (pid %d), %lu of %lu MiB free",
              timer_ms(), init_path, p->pid, mem.free_pages >> (20 - PAGE_SHIFT),
              mem.total_pages >> (20 - PAGE_SHIFT));
    int status = proc_reap(p);
    panic("init exited with status 0x%x", status);
}

__noreturn void kmain(void)
{
    timer_early_init();
    serial_init();
    console_init();
    boot_init();
    fb_screen_init();
    fbcon_init();
    kprintf("minios %s build %u (%s) booting, gcc %s\n", kernel_release, kernel_build_number,
            kernel_version, __VERSION__);

    boot_apply_cmdline();
    arch_init_cpu_boot();
    klog_ring_init();
    arch_init_traps();
    ksyms_init();
    arch_init_cpu_features();
    boot_log_environment();
#if CONFIG_TESTS
    ktest_run_stage(KTEST_EARLY);
#endif

    pmm_init();
    vmm_init();
    /* The APs are parked in bootloader reclaimable memory; move them onto
     * kernel stacks and page tables before that memory is freed. */
    smp_park_aps();
    pmm_reclaim_bootloader();
    slab_init();
#if CONFIG_TESTS
    ktest_run_stage(KTEST_MEMORY);
#endif
    initrd_init();
    vfs_init();
    initrdfs_init();
    devfs_init();
    mfs_init();
    fat_init();
    tmpfs_init();
    arch_init_interrupts();
    tlb_init();
    timer_init();
#if CONFIG_TESTS
    ktest_run_stage(KTEST_TIMER);
#endif
    rtc_init();
    console_tty_init();
    input_init();
    platform_devices_init();
    fbdev_init();
    pty_init();
    pci_init();
    /* /dev/devices, with the SMBIOS and ACPI table copies. */
    devinfo_init();
    blockdev_init();
    virtio_blk_init();
    swap_init();
    profile_init();
    lockstat_init();
    syscall_init();
    proc_init();
    futex_init();
    socket_init();
    sched_init();
    arch_irq_enable();
    smp_start_aps();

    console_start_daemon();

    if (!thread_create("kinit", kinit, NULL, 0))
        panic("cannot create kinit thread");
    sched_idle_loop();
}
