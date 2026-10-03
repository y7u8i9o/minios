#define KLOG_SUBSYS "trap"
#include <arch/trap.h>
#include <arch/frame.h>
#include <arch/irq.h>
#include <arch/syscall.h>
#include <console.h>
#include <mm/vmm.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <sched/sched.h>
#include <sched/user.h>
#include <ipc/signal.h>
#include <klog.h>
#include <debug/panic.h>

static const char *exception_name(uint64_t esr)
{
    switch (ESR_EC(esr)) {
    case EC_UNKNOWN: return "undefined instruction";
    case EC_SVC64: return "svc";
    case EC_IABT_LOWER: case EC_IABT_CUR: return "instruction abort";
    case EC_DABT_LOWER: case EC_DABT_CUR: return "data abort";
    case EC_BRK64: return "brk";
    case 0x22: return "pc alignment fault";
    case 0x26: return "sp alignment fault";
    case 0x07: return "fp or simd access";
    default: return "exception";
    }
}

/* Record an entry from EL0 for the panic dump and the profiler. */
static void record_user_entry(struct trapframe *tf)
{
    struct cpu *c = cpu_current();
    c->arch.last_user_esr = tf->esr;
    c->arch.last_user_pc = tf->pc;
    c->arch.last_user_sp = tf->sp;
    if (c->current)
        c->current->kentry_frame = tf;
}

/* Returning to user mode is the only preemption point for user threads. */
static void return_to_user(struct trapframe *tf)
{
    proc_exit_check();
    signal_deliver(tf);
    arch_irq_disable();
    sched_preempt();
}

/* An exception from EL0 that the kernel does not resolve: SIGSEGV, as on
 * x86_64, or the end of the process. */
static void user_exception(struct trapframe *tf)
{
    struct thread *t = thread_current();
    if (signal_fault(t->proc, SIGSEGV)) {
        return_to_user(tf);
        return;
    }
    klog_error("process %s (pid %d) killed by %s at pc %lx, esr %lx far %lx",
               t->proc->name, t->proc->pid, exception_name(tf->esr), tf->pc, tf->esr, tf->far);
    trap_dump_frame(tf);
    user_fault_exit(PROC_STATUS_SIGNALED(SIGSEGV));
}

void trap_dispatch(struct trapframe *tf)
{
    unsigned kind = tf->kind & 3;
    bool user = tf->kind & TRAP_LOWER;
    if (user)
        record_user_entry(tf);
    if (kind == TRAP_IRQ) {
        irq_dispatch(tf);
        if (user)
            return_to_user(tf);
        return;
    }
    unsigned ec = ESR_EC(tf->esr);
    if (user && kind == TRAP_SYNC && ec == EC_SVC64) {
        syscall_dispatch(tf);
        return;
    }
    bool abort = ec == EC_DABT_LOWER || ec == EC_DABT_CUR || ec == EC_IABT_LOWER || ec == EC_IABT_CUR;
    if (kind == TRAP_SYNC && abort) {
        /* Interrupts remain masked, as after the interrupt gate of a page
         * fault on x86_64. */
        if (vmm_handle_fault(tf, tf->far)) {
            if (user)
                return_to_user(tf);
            return;
        }
    }
    if (user) {
        user_exception(tf);
        return;
    }
    if (kind == TRAP_SYNC)
        panic_trap(tf, "unhandled exception: %s (class %02lx, esr %lx)",
                   exception_name(tf->esr), ESR_EC(tf->esr), tf->esr);
    panic_trap(tf, "unhandled %s", kind == TRAP_FIQ ? "fiq" : "serror");
}

void trap_dump_frame(const struct trapframe *tf)
{
    kprintf("%s, esr=%08lx far=%016lx\n", exception_name(tf->esr), tf->esr, tf->far);
    kprintf("PC=%016lx SP=%016lx PSTATE=%08lx\n", tf->pc, tf->sp, tf->pstate);
    for (int i = 0; i < 31; i += 3) {
        for (int j = i; j < i + 3 && j < 31; j++)
            kprintf("X%-2d=%016lx ", j, tf->x[j]);
        kprintf("\n");
    }
}

#define SYSREG(name) ({ uint64_t v_; __asm__ volatile("mrs %0, " #name : "=r"(v_)); v_; })

void trap_dump_extra(const struct trapframe *tf)
{
    kprintf("elr=%016lx spsr=%08lx sctlr=%08lx tcr=%016lx mair=%016lx\n",
            SYSREG(elr_el1), SYSREG(spsr_el1), SYSREG(sctlr_el1), SYSREG(tcr_el1), SYSREG(mair_el1));
    kprintf("ttbr0=%016lx ttbr1=%016lx vbar=%016lx tpidr=%016lx sp_el0=%016lx\n",
            SYSREG(ttbr0_el1), SYSREG(ttbr1_el1), SYSREG(vbar_el1), SYSREG(tpidr_el1), SYSREG(sp_el0));
}
