#include <arch/thread.h>
#include <arch/fpu.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <sched/thread.h>
#include <mm/slab.h>
#include <errno.h>

void context_switch(uint64_t **old_sp, uint64_t *new_sp);

int arch_thread_init(struct thread *t, void (*start)(void))
{
    t->arch.fpu_raw = kmalloc(FPU_AREA_SIZE + 16);
    if (!t->arch.fpu_raw)
        return -ENOMEM;
    t->arch.fpu = (void *)ALIGN_UP((uintptr_t)t->arch.fpu_raw, 16);
    fpu_init_state(t->arch.fpu);

    /* Initial frame: six callee saved registers then the return address
     * into start, laid out as context_switch expects. The address sits at
     * top - 16 so the stack is 16 byte aligned plus 8 on entry. */
    uint64_t *sp = (uint64_t *)t->kstack_top;
    *--sp = 0;                          /* padding */
    *--sp = (uint64_t)start;            /* return address */
    for (int i = 0; i < 6; i++)
        *--sp = 0;                      /* rbp, rbx, r12-r15 */
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
    wrmsr(MSR_FS_BASE, t->arch.tls_base);
}

void arch_set_kernel_stack(uintptr_t top)
{
    tss_set_rsp0(top);
}

uint64_t arch_get_tls(const struct thread *t)
{
    return t->arch.tls_base;
}

void arch_set_tls(struct thread *t, uint64_t base)
{
    t->arch.tls_base = base;
    if (t == thread_current())
        wrmsr(MSR_FS_BASE, base);
}

void arch_fpu_capture(struct thread *t)
{
    fpu_save(t->arch.fpu);
}

void arch_fpu_reset(struct thread *t)
{
    fpu_init_state(t->arch.fpu);
    fpu_restore(t->arch.fpu);
}

/* context_switch pushed rbp, rbx, r12 to r15 below the return address:
 * ctx[5] is the saved rbp and ctx[6] the return address. */
void arch_thread_switch_frame(const struct thread *t, uintptr_t *pc, uintptr_t *fp)
{
    *fp = t->ctx[5];
    *pc = t->ctx[6];
}
