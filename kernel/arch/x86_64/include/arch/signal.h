#pragma once
#include <kernel.h>

struct thread;
struct trapframe;

/* Save the interrupted user state of t (tf, the FPU state and saved_mask)
 * in a signal frame on the user stack, and redirect tf to handler(sig)
 * with restorer as its return address. Returns 0, or -EFAULT if the frame
 * does not fit in writable user memory. */
int arch_signal_setup_frame(struct trapframe *tf, struct thread *t, int sig,
                            uintptr_t handler, uintptr_t restorer, uint64_t saved_mask);
/* Restore the user state saved by arch_signal_setup_frame, as found by
 * the sigreturn system call issued from the restorer. Sanitizes the
 * privileged parts of the frame and stores the saved signal mask in
 * *saved_mask. Returns 0, or -EFAULT if the frame is not readable. */
int arch_signal_restore_frame(struct trapframe *tf, struct thread *t, uint64_t *saved_mask);
