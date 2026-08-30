#define KLOG_SUBSYS "signal"
#include <ipc/signal.h>
#include <arch/fpu.h>
#include <sched/proc.h>
#include <sched/thread.h>
#include <sched/user.h>
#include <sched/wait.h>
#include <arch/trap.h>
#include <arch/gdt.h>
#include <mm/vma.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define SIGBIT(s) (1UL << (s))
/* Signals whose default action is to ignore. */
#define DEFAULT_IGNORE (SIGBIT(SIGCHLD) | SIGBIT(SIGWINCH))
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

static bool valid_sig(int sig)
{
    return sig >= 1 && sig < NSIG;
}

/* Wake threads of p that would take the signal. Caller holds p->lock. */
static void interrupt_threads(struct proc *p, int sig)
{
    struct list_head *pos;
    list_for_each(pos, &p->threads) {
        struct thread *t = list_entry(pos, struct thread, proc_link);
        if (t != thread_current() && !(t->sig_mask & SIGBIT(sig)))
            waitq_interrupt(t);
    }
}

int signal_send(struct proc *p, int sig)
{
    if (!valid_sig(sig))
        return -EINVAL;
    if (p == &kernel_proc)
        return -EPERM;
    /* init is never terminated by a default action, only by a handler of
     * its own, so a stray kill cannot bring the system down. */
    bool is_init = p->pid == 1;
    if (sig == SIGKILL) {
        if (is_init)
            return 0;
        proc_begin_exit(p, PROC_STATUS_SIGNALED(SIGKILL));
        return 0;
    }
    spin_lock(&p->lock);
    void (*h)(int) = p->sig_actions[sig].handler;
    bool ignored = h == SIG_IGN || (h == SIG_DFL && ((DEFAULT_IGNORE & SIGBIT(sig)) || is_init));
    if (!ignored && !p->exiting) {
        p->sig_pending |= SIGBIT(sig);
        interrupt_threads(p, sig);
    }
    spin_unlock(&p->lock);
    klog_debug("signal %d to pid %d%s", sig, p->pid, ignored ? " (ignored)" : "");
    return 0;
}

int signal_send_pgrp(int pgid, int sig)
{
    int pids[64];
    int n = proc_collect_pgrp(pgid, pids, ARRAY_SIZE(pids));
    int sent = 0;
    for (int i = 0; i < n; i++) {
        struct proc *p = proc_find(pids[i]);
        if (p && signal_send(p, sig) == 0)
            sent++;
    }
    return sent;
}

bool signal_should_interrupt(void)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    if (p == &kernel_proc)
        return false;
    spin_lock(&p->lock);
    bool r = p->exiting || (p->sig_pending & ~t->sig_mask) != 0;
    spin_unlock(&p->lock);
    return r;
}

bool signal_fault(struct proc *p, int sig)
{
    spin_lock(&p->lock);
    void (*h)(int) = p->sig_actions[sig].handler;
    bool handled = h != SIG_DFL && h != SIG_IGN;
    if (handled)
        p->sig_pending |= SIGBIT(sig);
    spin_unlock(&p->lock);
    return handled;
}

void signal_reset_for_exec(struct proc *p)
{
    spin_lock(&p->lock);
    for (int s = 1; s < NSIG; s++) {
        if (p->sig_actions[s].handler != SIG_IGN)
            memset(&p->sig_actions[s], 0, sizeof p->sig_actions[s]);
    }
    spin_unlock(&p->lock);
}

void signal_copy(struct proc *dst, const struct proc *src)
{
    spin_lock(&dst->lock);
    memcpy(dst->sig_actions, src->sig_actions, sizeof dst->sig_actions);
    dst->sig_pending = 0;
    spin_unlock(&dst->lock);
}

/* Terminate the process because of sig's default action. */
static __noreturn void default_terminate(struct proc *p, int sig)
{
    klog_info("process %s (pid %d) terminated by signal %d", p->name, p->pid, sig);
    proc_begin_exit(p, PROC_STATUS_SIGNALED(sig));
    thread_exit(0);
}

void signal_deliver(struct trapframe *tf)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    if (p == &kernel_proc)
        return;
    spin_lock(&p->lock);
    uint64_t ready = p->sig_pending & ~t->sig_mask;
    if (!ready || p->exiting) {
        spin_unlock(&p->lock);
        return;
    }
    int sig = __builtin_ctzl(ready);
    p->sig_pending &= ~SIGBIT(sig);
    struct ksigaction act = p->sig_actions[sig];
    spin_unlock(&p->lock);
    klog_debug("deliver %d to pid %d handler %p", sig, p->pid, act.handler);

    if (act.handler == SIG_IGN)
        return;
    if (act.handler == SIG_DFL) {
        if (DEFAULT_IGNORE & SIGBIT(sig))
            return;
        default_terminate(p, sig);
    }

    /* Build the frame below the red zone, 16 byte aligned so the handler
     * sees rsp + 8 aligned as after a call. */
    uintptr_t sp = tf->rsp - 128 - sizeof(struct sigframe);
    sp = ALIGN_DOWN(sp, 16) - 8;
    if (!vma_range_ok(p->vm, sp, sizeof(struct sigframe), true)) {
        klog_error("process %s (pid %d): no room for a signal frame", p->name, p->pid);
        default_terminate(p, SIGSEGV);
    }
    struct sigframe frame;
    frame.restorer = (uint64_t)act.restorer;
    frame.tf = *tf;
    frame.saved_mask = t->sig_mask;
    frame.signo = (uint64_t)sig;
    fpu_save(t->fpu);
    memcpy(frame.fpu, t->fpu, FPU_AREA_SIZE);
    memcpy((void *)sp, &frame, sizeof frame);

    t->sig_mask |= act.mask;
    if (!(act.flags & SA_NODEFER))
        t->sig_mask |= SIGBIT(sig);
    t->sig_mask &= ~SIGBIT(SIGKILL);
    tf->rip = (uint64_t)act.handler;
    tf->rdi = (uint64_t)sig;
    tf->rsp = sp;
    tf->rax = 0;
}

long signal_return(struct trapframe *tf)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    /* The restorer runs after the handler's ret popped the return slot,
     * so rsp points at the saved trap frame. */
    uintptr_t base = tf->rsp - offsetof(struct sigframe, tf);
    if (!vma_range_ok(p->vm, base, sizeof(struct sigframe), false)) {
        klog_error("process %s (pid %d): bad sigreturn frame", p->name, p->pid);
        default_terminate(p, SIGSEGV);
    }
    struct sigframe frame;
    memcpy(&frame, (void *)base, sizeof frame);
    uint64_t rflags = (frame.tf.rflags & RFLAGS_USER_MASK) | RFLAGS_IF_BIT | 0x2;
    *tf = frame.tf;
    tf->cs = GDT_USER_CODE | 3;
    tf->ss = GDT_USER_DATA | 3;
    tf->rflags = rflags;
    t->sig_mask = frame.saved_mask & ~SIGBIT(SIGKILL);
    memcpy(t->fpu, frame.fpu, FPU_AREA_SIZE);
    fpu_restore(t->fpu);
    return (long)tf->rax;
}
