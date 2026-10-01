#pragma once
#include <kernel.h>

struct thread;

/* Architecture state of a thread, embedded in struct thread as t->arch.
 * Written by the thread itself and by the switch code of the CPU that
 * runs it; a new thread's state is filled by its creator before the
 * thread is queued. */
struct arch_thread {
    void *fpu;                      /* q0 to q31, FPCR and FPSR, 16 byte aligned inside fpu_raw */
    void *fpu_raw;
    uint64_t tls_base;              /* TPIDR_EL0 (thread local storage), loaded at every switch */
};

/* The operations are those of the x86_64 header; see docs/design/arch.md. */
int arch_thread_init(struct thread *t, void (*start)(void));
void arch_thread_free(struct thread *t);
/* The return address and frame pointer that context_switch saved for a
 * thread that is switched out, for /dev/threads. */
void arch_thread_switch_frame(const struct thread *t, uintptr_t *pc, uintptr_t *fp);
void arch_switch_to(struct thread *prev, struct thread *next);
void arch_thread_resume(struct thread *t);
void arch_set_kernel_stack(uintptr_t top);
uint64_t arch_get_tls(const struct thread *t);
void arch_set_tls(struct thread *t, uint64_t base);
void arch_fpu_capture(struct thread *t);
void arch_fpu_reset(struct thread *t);
