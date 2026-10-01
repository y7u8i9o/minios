#define KLOG_SUBSYS "boot"
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
#include <fs/fat.h>
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
