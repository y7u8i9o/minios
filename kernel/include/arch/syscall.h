#pragma once
#include <kernel.h>

struct trapframe;

/* Program STAR, LSTAR and SFMASK and enable syscall/sysret. */
void syscall_init(void);
/* The MSR part alone, run on every CPU. */
void syscall_init_cpu(void);
/* Called from syscall.S with the saved user state. */
void syscall_dispatch(struct trapframe *tf);
/* Jump to user mode with the given frame. Never returns. */
__noreturn void user_enter(struct trapframe *tf);
