#define KLOG_SUBSYS "syscall"
#include <arch/syscall.h>
#include <arch/cpu.h>
#include <arch/frame.h>
#include <sched/thread.h>
#include <syscall_nums.h>
#include <lib/string.h>
#include <klog.h>

/* The aarch64 side of user mode: entering EL0 and the system call entry
 * (svc, dispatched from trap.c). */

void trap_return(struct trapframe *tf) __attribute__((noreturn));

void syscall_init(void)
{
    klog_info("system calls through svc, %d calls", SYS_MAX - 1);
}

void syscall_init_cpu(void)
{
}

void arch_syscall_enter(struct trapframe *tf)
{
    struct cpu *c = cpu_current();
    c->arch.last_user_esr = tf->esr;
    c->arch.last_user_pc = tf->pc;
    c->arch.last_user_sp = tf->sp;
}

/* eret restores every register, so a rewritten frame needs no other path. */
void arch_syscall_return_full(struct trapframe *tf)
{
    (void)tf;
}

/* The frame is moved to the top of the thread's kernel stack, so that
 * SP_EL1 is the stack top while the thread runs in EL0. The move cannot
 * overwrite the stack this function runs on: tf lies in a caller's frame,
 * above it. */
__noreturn void user_enter(struct trapframe *tf)
{
    struct trapframe *top = (struct trapframe *)((uintptr_t)cpu_current()->kstack_top - sizeof *top);
    if (top != tf)
        memmove(top, tf, sizeof *top);
    top->kind = TRAP_SYNC | TRAP_LOWER;
    top->pstate = 0;                    /* EL0t, interrupts unmasked */
    trap_return(top);
}
