#include <arch/signal.h>
#include <arch/fpu.h>
#include <arch/gdt.h>
#include <arch/trap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <lib/string.h>
#include <errno.h>

/* User controllable RFLAGS bits restored by sigreturn. */
#define RFLAGS_USER_MASK 0xcd5UL
#define RFLAGS_IF_BIT   (1UL << 9)

/* Frame pushed on the user stack for a handler. The return address slot
 * makes the handler's ret land in the libc restorer, which issues
 * sigreturn with rsp pointing at tf. */
struct sigframe {
    uint64_t restorer;
    struct trapframe tf;
    uint64_t saved_mask;
    uint64_t signo;
    uint8_t fpu[FPU_AREA_SIZE];     /* the interrupted FPU and SSE state (M23) */
};

int arch_signal_setup_frame(struct trapframe *tf, struct thread *t, int sig,
                            uintptr_t handler, uintptr_t restorer, uint64_t saved_mask)
{
    /* Build the frame below the red zone, 16 byte aligned so the handler
     * sees rsp + 8 aligned as after a call. */
    uintptr_t sp = tf->rsp - 128 - sizeof(struct sigframe);
    sp = ALIGN_DOWN(sp, 16) - 8;
    if (!vma_range_ok(t->proc->vm, sp, sizeof(struct sigframe), true))
        return -EFAULT;
    struct sigframe frame;
    frame.restorer = restorer;
    frame.tf = *tf;
    frame.saved_mask = saved_mask;
    frame.signo = (uint64_t)sig;
    fpu_save(t->arch.fpu);
    memcpy(frame.fpu, t->arch.fpu, FPU_AREA_SIZE);
    memcpy((void *)sp, &frame, sizeof frame);

    tf->rip = handler;
    tf->rdi = (uint64_t)sig;
    tf->rsp = sp;
    tf->rax = 0;
    return 0;
}

int arch_signal_restore_frame(struct trapframe *tf, struct thread *t, uint64_t *saved_mask)
{
    /* The restorer runs after the handler's ret popped the return slot,
     * so rsp points at the saved trap frame. */
    uintptr_t base = tf->rsp - offsetof(struct sigframe, tf);
    if (!vma_range_ok(t->proc->vm, base, sizeof(struct sigframe), false))
        return -EFAULT;
    struct sigframe frame;
    memcpy(&frame, (void *)base, sizeof frame);
    uint64_t rflags = (frame.tf.rflags & RFLAGS_USER_MASK) | RFLAGS_IF_BIT | 0x2;
    *tf = frame.tf;
    tf->cs = GDT_USER_CODE | 3;
    tf->ss = GDT_USER_DATA | 3;
    tf->rflags = rflags;
    *saved_mask = frame.saved_mask;
    memcpy(t->arch.fpu, frame.fpu, FPU_AREA_SIZE);
    fpu_restore(t->arch.fpu);
    return 0;
}
