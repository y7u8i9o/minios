#pragma once
#include <kernel.h>

struct trapframe;

/* Program STAR, LSTAR and SFMASK and enable syscall/sysret. */
void syscall_init(void);
/* The MSR part alone, run on every CPU. */
void syscall_init_cpu(void);
/* Generic dispatch (syscall/table.c), called from syscall.S with the
 * saved user state. */
void syscall_dispatch(struct trapframe *tf);
/* Record the entry for diagnostics. Called first by syscall_dispatch with
 * interrupts still disabled. */
void arch_syscall_enter(struct trapframe *tf);
/* Called when a system call replaced the whole user register state
 * (sigreturn). An architecture whose fast return path cannot restore every
 * register enters user mode here and does not return. */
void arch_syscall_return_full(struct trapframe *tf);
/* Jump to user mode with the given frame. Never returns. */
__noreturn void user_enter(struct trapframe *tf);
