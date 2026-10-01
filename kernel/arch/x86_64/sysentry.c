#define KLOG_SUBSYS "syscall"
#include <arch/syscall.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/trap.h>
#include <syscall_nums.h>
#include <klog.h>

/* The x86_64 side of system call entry: the syscall/sysret MSRs and the
 * diagnostics recorded at each entry. The dispatch itself is generic
 * (syscall/table.c). */

#define EFER_SCE 1UL
#define RFLAGS_TF (1UL << 8)
#define RFLAGS_DF (1UL << 10)
#define RFLAGS_AC (1UL << 18)

void syscall_entry(void);

void syscall_init_cpu(void)
{
    /* STAR: syscall loads CS=0x08, SS=0x10. sysret loads CS from
     * STAR[63:48]+16 and SS from STAR[63:48]+8. Intel processors OR 3
     * into both selectors, AMD processors do not, so the base carries
     * RPL 3 itself: 0x13 gives CS=0x23 and SS=0x1b on both. With 0x10 an
     * AMD processor ran user code with SS=0x18, and the next iretq back
     * to user mode faulted with #GP(0x18). */
    wrmsr(MSR_STAR, ((uint64_t)(GDT_KERNEL_DATA | 3) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_TF | RFLAGS_DF | RFLAGS_AC);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void syscall_init(void)
{
    syscall_init_cpu();
    klog_info("syscall entry at %p, %d calls", syscall_entry, SYS_MAX - 1);
}

void arch_syscall_enter(struct trapframe *tf)
{
    struct cpu *c = cpu_current();
    c->arch.last_user_vector = tf->vector;
    c->arch.last_user_error = 0;
    c->arch.last_user_rip = tf->rip;
    c->arch.last_user_cs = tf->cs;
    c->arch.last_user_rsp = tf->rsp;
    c->arch.last_user_ss = tf->ss;
    c->arch.last_user_cr3 = read_cr3();
    c->arch.last_user_frame = (void *)tf;
}

void arch_syscall_return_full(struct trapframe *tf)
{
    /* sysret takes RIP from RCX and RFLAGS from R11, so it cannot restore
     * those two registers. A frame rewritten by sigreturn belongs to code
     * that may have been interrupted anywhere, so it is entered through
     * the iretq path, which restores every register. */
    user_enter(tf);
}
