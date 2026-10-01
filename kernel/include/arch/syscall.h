#pragma once
#include <kernel.h>

struct trapframe;

/* System call entry (docs/design/arch.md). Shared by every architecture;
 * the functions other than syscall_dispatch are implemented by the
 * architecture.
 *
 * Enable the system call instruction (on x86_64 the STAR, LSTAR and SFMASK
 * MSRs). */
void syscall_init(void);
/* The per CPU part of syscall_init, run on every CPU. */
void syscall_init_cpu(void);
/* Generic dispatch (syscall/table.c), called from the entry code of the
 * architecture with the saved user state. */
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
