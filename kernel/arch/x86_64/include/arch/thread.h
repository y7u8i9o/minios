#pragma once
#include <kernel.h>

struct thread;

/* Architecture state of a thread, embedded in struct thread as t->arch.
 * Written by the thread itself and by the switch code of the CPU that
 * runs it; a new thread's state is filled by its creator before the
 * thread is queued. */
struct arch_thread {
    void *fpu;                      /* fxsave area, 16 byte aligned inside fpu_raw (M23) */
    void *fpu_raw;
    uint64_t tls_base;              /* user FS base (thread local storage), loaded at every switch (M35) */
};

/* Allocate the FPU state of t in its initial contents and prepare t's
 * kernel stack so that the first switch to t enters start. Returns 0 or
 * -ENOMEM. */
int arch_thread_init(struct thread *t, void (*start)(void));
/* Release what arch_thread_init allocated. */
void arch_thread_free(struct thread *t);

/* Save prev's user register state and switch to next's kernel stack.
 * Returns when prev is switched back to. */
void arch_switch_to(struct thread *prev, struct thread *next);
/* Load the user register state of t (FPU, TLS base) into the calling CPU.
 * Called after every switch and by a new thread's first code. */
void arch_thread_resume(struct thread *t);
/* Use top as the kernel stack for entries from user mode. */
void arch_set_kernel_stack(uintptr_t top);

/* The TLS base of t. arch_set_tls also loads the register when t is the
 * calling thread. */
uint64_t arch_get_tls(const struct thread *t);
void arch_set_tls(struct thread *t, uint64_t base);

/* Store the FPU registers of the calling CPU in t's area: the child of
 * fork starts with the current FPU register contents of its parent. */
void arch_fpu_capture(struct thread *t);
/* Reset the FPU state of the calling thread t to the initial contents and
 * load it (execve). */
void arch_fpu_reset(struct thread *t);
