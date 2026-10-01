#include <syscall/syscalls.h>
#include <arch/platform.h>
#include <arch/cpu.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <sched/sched.h>
#include <ipc/signal.h>
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <mm/vma.h>
#include <sync/rcu.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <console.h>
#include <klog.h>
#include <errno.h>

long sys_sigaction(struct trapframe *tf)
{
    int sig = (int)SYSARG0(tf);
    uintptr_t act = SYSARG1(tf), oldact = SYSARG2(tf);
    if (sig < 1 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;
    if (act && !user_range_ok(act, sizeof(struct sigaction), false))
        return -EFAULT;
    if (oldact && !user_range_ok(oldact, sizeof(struct sigaction), true))
        return -EFAULT;
    struct sigaction copy;
    if (act)
        memcpy(&copy, (void *)act, sizeof copy);
    struct proc *p = thread_current()->proc;
    spin_lock(&p->lock);
    struct ksigaction old = p->sig_actions[sig];
    if (act) {
        p->sig_actions[sig].handler = copy.sa_handler;
        p->sig_actions[sig].mask = copy.sa_mask;
        p->sig_actions[sig].flags = copy.sa_flags;
        p->sig_actions[sig].restorer = copy.sa_restorer;
        if (copy.sa_handler == SIG_IGN)
            __atomic_fetch_and(&p->sig_pending, ~(1UL << sig), __ATOMIC_RELEASE);
    }
    spin_unlock(&p->lock);
    if (oldact) {
        struct sigaction out = { old.handler, old.mask, old.flags, old.restorer };
        memcpy((void *)oldact, &out, sizeof out);
    }
    return 0;
}

long sys_sigprocmask(struct trapframe *tf)
{
    int how = (int)SYSARG0(tf);
    uintptr_t set = SYSARG1(tf), oldset = SYSARG2(tf);
    if (set && !user_range_ok(set, sizeof(sigset_t), false))
        return -EFAULT;
    if (oldset && !user_range_ok(oldset, sizeof(sigset_t), true))
        return -EFAULT;
    struct thread *t = thread_current();
    uint64_t old = t->sig_mask;
    if (set) {
        uint64_t s = *(uint64_t *)set;
        switch (how) {
        case SIG_BLOCK:   t->sig_mask |= s; break;
        case SIG_UNBLOCK: t->sig_mask &= ~s; break;
        case SIG_SETMASK: t->sig_mask = s; break;
        default: return -EINVAL;
        }
        t->sig_mask &= ~((1UL << SIGKILL) | (1UL << SIGSTOP));
    }
    if (oldset)
        *(uint64_t *)oldset = old;
    return 0;
}

long sys_sigreturn(struct trapframe *tf)
{
    return signal_return(tf);
}

/* kill(pid, sig): pid > 0 one process, 0 the caller's group, -1 every
 * process except init, < -1 the group -pid. sig 0 only checks. */
long sys_kill(struct trapframe *tf)
{
    int pid = (int)SYSARG0(tf);
    int sig = (int)SYSARG1(tf);
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;
    struct proc *self = thread_current()->proc;
    if (pid > 0) {
        struct proc *p = proc_find(pid);
        if (!p || p == &kernel_proc)
            return -ESRCH;
        return sig ? signal_send(p, sig) : 0;
    }
    int pgid;
    if (pid == 0) {
        spin_lock(&proc_tree_lock);
        pgid = self->pgid;
        spin_unlock(&proc_tree_lock);
    } else if (pid == -1) {
        pgid = 0;
    } else {
        pgid = -pid;
    }
    if (!sig)
        return 0;
    int n = signal_send_pgrp(pgid, sig);
    return n ? 0 : -ESRCH;
}

long sys_setpgid(struct trapframe *tf)
{
    int pid = (int)SYSARG0(tf), pgid = (int)SYSARG1(tf);
    struct proc *self = thread_current()->proc;
    struct proc *p = pid ? proc_find(pid) : self;
    if (!p || p == &kernel_proc)
        return -ESRCH;
    if (pgid < 0)
        return -EINVAL;
    spin_lock(&proc_tree_lock);
    if (p != self && p->parent != self) {
        spin_unlock(&proc_tree_lock);
        return -EPERM;
    }
    p->pgid = pgid ? pgid : p->pid;
    spin_unlock(&proc_tree_lock);
    return 0;
}

long sys_getpgid(struct trapframe *tf)
{
    int pid = (int)SYSARG0(tf);
    struct proc *p = pid ? proc_find(pid) : thread_current()->proc;
    if (!p)
        return -ESRCH;
    spin_lock(&proc_tree_lock);
    int pgid = p->pgid;
    spin_unlock(&proc_tree_lock);
    return pgid;
}

/* tcsetpgrp(fd, pgid) and tcgetpgrp(fd) address the terminal behind fd
 * through its ioctl. */
static long tty_request(int fd, unsigned long req, uintptr_t arg)
{
    struct file *f = fdtable_get(&thread_current()->proc->fds, fd);
    if (!f)
        return -EBADF;
    long r = f->ops && f->ops->ioctl ? f->ops->ioctl(f, req, arg) : -ENOTTY;
    file_put(f);
    return r;
}

long sys_tcsetpgrp(struct trapframe *tf)
{
    return tty_request((int)SYSARG0(tf), TIOCSPGRP, SYSARG1(tf));
}

long sys_tcgetpgrp(struct trapframe *tf)
{
    return tty_request((int)SYSARG0(tf), TIOCGPGRP, 0);
}

/* reboot(cmd): only init may call it. Every other process gets SIGTERM,
 * then SIGKILL after a grace period; filesystems are flushed and
 * unmounted; then the machine powers off, reboots or halts. */
long sys_reboot(struct trapframe *tf)
{
    int cmd = (int)SYSARG0(tf);
    struct proc *self = thread_current()->proc;
    if (cmd != RB_POWER_OFF && cmd != RB_AUTOBOOT && cmd != RB_HALT)
        return -EINVAL;
    if (self->pid != 1)
        return -EPERM;
    klog_info("reboot(%d) requested by init", cmd);
    signal_send_pgrp(0, SIGTERM);
    for (int i = 0; i < 40 && proc_count_others() > 0; i++)
        sleep_ms(50);
    if (proc_count_others() > 0) {
        char table[1024];
        proc_format_table(table, sizeof table);
        klog_warn("%d processes ignored SIGTERM, sending SIGKILL\n%s", proc_count_others(), table);
        signal_send_pgrp(0, SIGKILL);
        for (int i = 0; i < 20 && proc_count_others() > 0; i++)
            sleep_ms(50);
    }
    /* Init never returns to user mode from here, so its own file mappings
     * (the shared libraries of a dynamic init) can go; the regions of the
     * processes just reaped release their files through RCU callbacks,
     * which must have run before the filesystems are judged busy. */
    vma_remove_all(self->vm);
    rcu_synchronize();
    int busy = vfs_umount_all();
    if (busy)
        klog_warn("%d filesystems were busy", busy);
    /* Console output is queued per CPU and drained by a thread that never
     * runs again once the machine stops: write the final lines out here. */
    switch (cmd) {
    case RB_AUTOBOOT:
        kprintf("system rebooting\n");
        console_flush();
        platform_reboot();
    case RB_HALT:
        kprintf("system halted\n");
        console_flush();
        arch_irq_disable();
        cpu_halt_forever();
    default:
        kprintf("system powering off\n");
        console_flush();
        platform_power_off();
    }
}
