#include <arch/thread.h>
#include <arch/cpu.h>
#include <sched/thread.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <errno.h>
#include "fpu.h"

void context_switch(uint64_t **old_sp, uint64_t *new_sp);

int arch_thread_init(struct thread *t, void (*start)(void))
{
    t->arch.fpu_raw = kmalloc(FPU_AREA_SIZE + 16);
    if (!t->arch.fpu_raw)
        return -ENOMEM;
    t->arch.fpu = (void *)ALIGN_UP((uintptr_t)t->arch.fpu_raw, 16);
    memset(t->arch.fpu, 0, FPU_AREA_SIZE);  /* zero registers, FPCR round to nearest */

    /* Initial frame: x19 to x30 as context_switch saves them, x30 the
     * first function and x29 zero, which ends backtraces. */
    uint64_t *sp = (uint64_t *)t->kstack_top - 12;
    memset(sp, 0, 12 * sizeof *sp);
    sp[11] = (uint64_t)start;
    t->ctx = sp;
    return 0;
}

void arch_thread_free(struct thread *t)
{
    kfree(t->arch.fpu_raw);
}

void arch_switch_to(struct thread *prev, struct thread *next)
{
    if (prev->arch.fpu)
        fpu_save(prev->arch.fpu);
    context_switch(&prev->ctx, next->ctx);
}

void arch_thread_resume(struct thread *t)
{
    if (t->arch.fpu)
        fpu_restore(t->arch.fpu);
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(t->arch.tls_base));
}

/* An exception from EL0 is taken on SP_EL1, which keeps the value it had
 * at the eret to EL0: user_enter returns from the top of the thread's
 * kernel stack, so nothing needs to be loaded here. */
void arch_set_kernel_stack(uintptr_t top)
{
    (void)top;
}

uint64_t arch_get_tls(const struct thread *t)
{
    return t->arch.tls_base;
}

void arch_set_tls(struct thread *t, uint64_t base)
{
    t->arch.tls_base = base;
    if (t == thread_current())
        __asm__ volatile("msr tpidr_el0, %0" : : "r"(base));
}

void arch_fpu_capture(struct thread *t)
{
    fpu_save(t->arch.fpu);
}

void arch_fpu_reset(struct thread *t)
{
    memset(t->arch.fpu, 0, FPU_AREA_SIZE);
    fpu_restore(t->arch.fpu);
}

/* context_switch stored x29 and x30 at offset 80 of its frame. */
void arch_thread_switch_frame(const struct thread *t, uintptr_t *pc, uintptr_t *fp)
{
    *fp = t->ctx[10];
    *pc = t->ctx[11];
}
