#include <arch/signal.h>
#include <arch/trap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <lib/string.h>
#include <errno.h>
#include "fpu.h"

/* Frame pushed on the user stack for a handler. The handler is entered
 * with the signal number in x0 and the restorer in x30, so its return
 * runs the restorer, which issues sigreturn with sp unchanged: sp then
 * points at the frame. */
struct sigframe {
    struct trapframe tf;
    uint64_t saved_mask;
    uint64_t signo;
    uint8_t fpu[FPU_AREA_SIZE];
} __aligned(16);

/* NZCV, the only PSTATE bits a user frame may set. */
#define PSTATE_USER_MASK 0xf0000000UL

int arch_signal_setup_frame(struct trapframe *tf, struct thread *t, int sig,
                            uintptr_t handler, uintptr_t restorer, uint64_t saved_mask)
{
    uintptr_t sp = ALIGN_DOWN(tf->sp - sizeof(struct sigframe), 16);
    if (!vma_range_ok(t->proc->vm, sp, sizeof(struct sigframe), true))
        return -EFAULT;
    struct sigframe frame;
    frame.tf = *tf;
    frame.saved_mask = saved_mask;
    frame.signo = (uint64_t)sig;
    fpu_save(t->arch.fpu);
    memcpy(frame.fpu, t->arch.fpu, FPU_AREA_SIZE);
    memcpy((void *)sp, &frame, sizeof frame);

    tf->pc = handler;
    tf->x[0] = (uint64_t)sig;
    tf->x[30] = restorer;
    tf->sp = sp;
    return 0;
}

int arch_signal_restore_frame(struct trapframe *tf, struct thread *t, uint64_t *saved_mask)
{
    uintptr_t base = tf->sp;
    if (!vma_range_ok(t->proc->vm, base, sizeof(struct sigframe), false))
        return -EFAULT;
    struct sigframe frame;
    memcpy(&frame, (void *)base, sizeof frame);
    uint64_t kind = tf->kind;
    *tf = frame.tf;
    tf->pstate &= PSTATE_USER_MASK;     /* EL0t, interrupts unmasked */
    tf->kind = kind;
    *saved_mask = frame.saved_mask;
    memcpy(t->arch.fpu, frame.fpu, FPU_AREA_SIZE);
    fpu_restore(t->arch.fpu);
    return 0;
}
