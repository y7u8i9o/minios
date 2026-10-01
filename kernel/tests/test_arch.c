#include <tests/ktest.h>
#include <arch/frame.h>
#include <arch/cpu.h>
#include <arch/thread.h>
#include <arch/machine.h>
#include <syscall/syscalls.h>
#include <sched/thread.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <console.h>
#include <errno.h>

/* A0: the architecture interface (docs/design/arch.md). Generic code
 * reaches the saved register state, the interrupt state, the TLS base and
 * the machine identification only through these operations, so each is
 * checked against the behaviour the generic callers rely on. */

static void check_frame(void)
{
    struct trapframe tf;
    memset(&tf, 0xa5, sizeof tf);
    arch_frame_init_user(&tf, 0x401000, 0x7fffffffe000);
    ktest_assert(frame_pc(&tf) == 0x401000, "user frame pc %lx", frame_pc(&tf));
    ktest_assert(frame_sp(&tf) == 0x7fffffffe000, "user frame sp %lx", frame_sp(&tf));
    ktest_assert(frame_from_user(&tf), "user frame not marked as user mode");
    ktest_assert(frame_fp(&tf) == 0 && frame_retval(&tf) == 0 && SYSARG0(&tf) == 0,
                 "user frame registers not cleared");

    frame_set_pc(&tf, 0x402000);
    frame_set_sp(&tf, 0x7fffffffd000);
    frame_set_arg0(&tf, 17);
    frame_set_retval(&tf, (uint64_t)-EINTR);
    ktest_assert(frame_pc(&tf) == 0x402000 && frame_sp(&tf) == 0x7fffffffd000, "pc or sp not stored");
    ktest_assert(SYSARG0(&tf) == 17, "arg0 is not the first system call argument");
    ktest_assert((long)frame_retval(&tf) == -EINTR, "return value %ld", (long)frame_retval(&tf));
#if defined(__x86_64__)
    /* On x86_64 the number and the result share rax: a system call that
     * is restarted must find its number replaced by the result. */
    ktest_assert(frame_syscall_nr(&tf) == frame_retval(&tf), "number and result registers differ");
#endif

    struct trapframe kf;
    memset(&kf, 0, sizeof kf);
    ktest_assert(!frame_from_user(&kf), "zeroed frame marked as user mode");

    /* A write from user mode to a present page, and an instruction fetch
     * from an unmapped kernel page. */
    struct fault_info fi;
#if defined(__x86_64__)
    kf.error = 0x7;                     /* #PF error code: present, write, user */
#elif defined(__aarch64__)
    kf.esr = (0x24UL << 26) | (1UL << 6) | 0x0f;   /* data abort from EL0, WnR, permission fault */
#endif
    arch_fault_decode(&kf, &fi);
    ktest_assert(fi.present && fi.write && fi.user && !fi.exec, "user write fault decoded wrongly");
#if defined(__x86_64__)
    kf.error = 0x10;                    /* instruction fetch */
#elif defined(__aarch64__)
    kf.esr = (0x21UL << 26) | 0x07;     /* instruction abort at EL1, translation fault */
#endif
    arch_fault_decode(&kf, &fi);
    ktest_assert(!fi.present && !fi.write && !fi.user && fi.exec, "kernel fetch fault decoded wrongly");
}

static void check_irq_state(void)
{
    ktest_assert(arch_irqs_enabled(), "test thread runs with interrupts disabled");
    push_cli();
    push_cli();
    ktest_assert(!arch_irqs_enabled(), "push_cli left interrupts enabled");
    pop_cli();
    ktest_assert(!arch_irqs_enabled(), "inner pop_cli enabled interrupts");
    pop_cli();
    ktest_assert(arch_irqs_enabled(), "outer pop_cli did not restore interrupts");

    unsigned long outer = arch_irq_save();
    ktest_assert(!arch_irqs_enabled(), "arch_irq_save left interrupts enabled");
    unsigned long inner = arch_irq_save();
    arch_irq_restore(inner);
    ktest_assert(!arch_irqs_enabled(), "inner arch_irq_restore enabled interrupts");
    arch_irq_restore(outer);
    ktest_assert(arch_irqs_enabled(), "outer arch_irq_restore did not enable interrupts");

    uint64_t c0 = arch_cycles();
    uint64_t c1 = arch_cycles();
    ktest_assert(c1 >= c0, "cycle counter went backwards");
}

static void check_tls(void)
{
    struct thread *t = thread_current();
    uint64_t saved = arch_get_tls(t);
    arch_set_tls(t, 0x12345000);
    ktest_assert(arch_get_tls(t) == 0x12345000, "TLS base not stored");
#if defined(__x86_64__)
    uint64_t loaded = rdmsr(MSR_FS_BASE);
#elif defined(__aarch64__)
    uint64_t loaded;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(loaded));
#endif
    ktest_assert(loaded == 0x12345000, "TLS base not loaded for the calling thread");
    arch_set_tls(t, saved);
}

/* The machine number is the one the ELF loader accepts, so the e_machine
 * field of init on the root filesystem must equal it. */
static void check_machine(void)
{
    struct file *f;
    ktest_assert(vfs_open("/bin/init", O_RDONLY, 0, &f) == 0, "open /bin/init");
    uint8_t eh[20];
    long n = file_read(f, (char *)eh, sizeof eh);
    file_put(f);
    ktest_assert(n == (long)sizeof eh, "read %ld bytes of the ELF header", n);
    uint16_t machine = (uint16_t)(eh[18] | eh[19] << 8);
    ktest_assert(machine == ARCH_ELF_MACHINE, "/bin/init e_machine %u, kernel accepts %u",
                 machine, ARCH_ELF_MACHINE);
    kprintf("arch: %s, ELF machine %u\n", ARCH_MACHINE_NAME, ARCH_ELF_MACHINE);
}

static void test_arch(void)
{
    check_frame();
    check_irq_state();
    check_tls();
    check_machine();
}
KTEST_DEFINE("arch", test_arch);
