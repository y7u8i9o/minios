/* Interface functions whose aarch64 implementation belongs to later
 * milestones of docs/plan/arm64.md: threads, user mode, system calls and
 * signals (A6), and the application processors (A8). */
#include <arch/init.h>
#include <arch/irq.h>
#include <arch/smp.h>
#include <arch/syscall.h>
#include <arch/signal.h>
#include <arch/thread.h>
#include <arch/trap.h>
#include "todo.h"


int arch_thread_init(struct thread *t, void (*start)(void)) { ARCH_TODO("A6"); }
void arch_thread_free(struct thread *t) { ARCH_TODO("A6"); }
void arch_switch_to(struct thread *prev, struct thread *next) { ARCH_TODO("A6"); }
void arch_thread_resume(struct thread *t) { ARCH_TODO("A6"); }
void arch_set_kernel_stack(uintptr_t top) { ARCH_TODO("A6"); }
uint64_t arch_get_tls(const struct thread *t) { ARCH_TODO("A6"); }
void arch_set_tls(struct thread *t, uint64_t base) { ARCH_TODO("A6"); }
void arch_fpu_capture(struct thread *t) { ARCH_TODO("A6"); }
void arch_fpu_reset(struct thread *t) { ARCH_TODO("A6"); }

void syscall_init(void) { ARCH_TODO("A6"); }
void syscall_init_cpu(void) { ARCH_TODO("A6"); }
void arch_syscall_enter(struct trapframe *tf) { ARCH_TODO("A6"); }
void arch_syscall_return_full(struct trapframe *tf) { ARCH_TODO("A6"); }
__noreturn void user_enter(struct trapframe *tf) { ARCH_TODO("A6"); }
int arch_signal_setup_frame(struct trapframe *tf, struct thread *t, int sig,
                            uintptr_t handler, uintptr_t restorer, uint64_t saved_mask) { ARCH_TODO("A6"); }
int arch_signal_restore_frame(struct trapframe *tf, struct thread *t, uint64_t *saved_mask) { ARCH_TODO("A6"); }

/* One processor until A8. */
void smp_park_aps(void) {}
void smp_start_aps(void) {}
unsigned smp_cpu_count(void) { return 1; }
cpu_mask_t smp_online_mask(void) { return 1; }
bool smp_active(void) { return false; }
void smp_halt_others(void) {}
