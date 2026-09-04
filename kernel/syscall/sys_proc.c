#include <syscall/syscalls.h>
#include <arch/trap.h>
#include <arch/cpu.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <sched/sched.h>
#include <sched/user.h>
#include <ipc/signal.h>
#include <mm/vma.h>
#include <syscall/rlimit.h>
#include <lib/string.h>
#include <errno.h>

long sys_exit(struct trapframe *tf)
{
    proc_begin_exit(thread_current()->proc, PROC_STATUS_EXITED((int)SYSARG0(tf)));
    thread_exit(0);
}

long sys_getpid(struct trapframe *tf)
{
    return thread_current()->proc->pid;
}

long sys_getppid(struct trapframe *tf)
{
    struct proc *p = thread_current()->proc;
    spin_lock(&proc_tree_lock);
    int ppid = p->parent ? p->parent->pid : 0;
    spin_unlock(&proc_tree_lock);
    return ppid;
}

long sys_yield(struct trapframe *tf)
{
    sched_yield();
    return 0;
}

long sys_fork(struct trapframe *tf)
{
    struct proc *p = thread_current()->proc;
    uint64_t lim = proc_rlimit_cur(p, RLIMIT_NPROC);
    if (lim != RLIM_INFINITY && (uint64_t)proc_count_users() >= lim)
        return -EAGAIN;
    struct proc *child = proc_fork(tf);
    return child ? child->pid : -ENOMEM;
}

long sys_execve(struct trapframe *tf)
{
    char path[USER_PATH_MAX];
    long r = copy_string_from_user(path, SYSARG0(tf), sizeof path);
    if (r < 0)
        return r;
    char **argv = NULL, **envp = NULL;
    r = copy_vector_from_user(SYSARG1(tf), &argv);
    if (r < 0)
        return r;
    r = copy_vector_from_user(SYSARG2(tf), &envp);
    if (r < 0) {
        free_user_vector(argv);
        return r;
    }
    r = proc_exec(tf, path, argv, envp);
    free_user_vector(argv);
    free_user_vector(envp);
    return r;
}

#define WNOHANG 1

/* wait4(pid, status, options, rusage): pid -1 waits for any child, pid
 * < -1 for any child in that process group. WNOHANG returns 0 instead of
 * blocking. */
long sys_wait4(struct trapframe *tf)
{
    int pid = (int)SYSARG0(tf);
    uintptr_t status_ptr = SYSARG1(tf);
    int options = (int)SYSARG2(tf);
    uintptr_t rusage_ptr = SYSARG3(tf);
    struct proc *self = thread_current()->proc;
    if (status_ptr && !user_range_ok(status_ptr, sizeof(int), true))
        return -EFAULT;
    if (rusage_ptr && !user_range_ok(rusage_ptr, sizeof(struct rusage), true))
        return -EFAULT;

    spin_lock(&proc_tree_lock);
    for (;;) {
        struct list_head *pos;
        bool any = false;
        struct proc *zombie = NULL;
        list_for_each(pos, &self->children) {
            struct proc *c = list_entry(pos, struct proc, sibling);
            if (pid > 0 && c->pid != pid)
                continue;
            if (pid < -1 && c->pgid != -pid)
                continue;
            any = true;
            if (c->state == PROC_ZOMBIE) {
                zombie = c;
                break;
            }
        }
        if (zombie) {
            list_del(&zombie->sibling);
            list_init(&zombie->sibling);
            spin_unlock(&proc_tree_lock);
            int cpid = zombie->pid;
            struct rusage ru;
            if (rusage_ptr)
                rusage_of_proc(zombie, &ru, false);
            int status = proc_reap(zombie);
            if (status_ptr)
                *(int *)status_ptr = status;
            if (rusage_ptr)
                *(struct rusage *)rusage_ptr = ru;
            return cpid;
        }
        if (!any) {
            spin_unlock(&proc_tree_lock);
            return -ECHILD;
        }
        if (options & WNOHANG) {
            spin_unlock(&proc_tree_lock);
            return 0;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&proc_tree_lock);
            return -EINTR;
        }
        waitq_wait(&self->child_waitq, &proc_tree_lock);
    }
}

long sys_thread_create(struct trapframe *tf)
{
    uintptr_t entry = SYSARG0(tf), arg = SYSARG1(tf), stack = SYSARG2(tf);
    if (!user_range_ok(entry, 1, false) || !user_range_ok(stack - 16, 16, true))
        return -EFAULT;
    return user_thread_create(entry, arg, stack);
}

long sys_thread_exit(struct trapframe *tf)
{
    thread_exit((int)SYSARG0(tf));
}

long sys_gettid(struct trapframe *tf)
{
    return thread_current()->tid;
}

/* set_tls(base): the FS base of the calling thread, the anchor of its
 * thread local storage.  Every later switch to the thread reloads it. */
long sys_set_tls(struct trapframe *tf)
{
    uintptr_t base = SYSARG0(tf);
    if (base && !user_range_ok(base, 8, false))
        return -EFAULT;
    struct thread *t = thread_current();
    t->fs_base = base;
    wrmsr(MSR_FS_BASE, base);
    return 0;
}

long sys_thread_join(struct trapframe *tf)
{
    int tid = (int)SYSARG0(tf);
    uintptr_t code_ptr = SYSARG1(tf);
    struct proc *p = thread_current()->proc;
    if (code_ptr && !user_range_ok(code_ptr, sizeof(int), true))
        return -EFAULT;

    /* Find the thread among the live or exited threads of the process. */
    struct thread *target = NULL;
    spin_lock(&p->lock);
    for (;;) {
        struct list_head *pos;
        list_for_each(pos, &p->zombies) {
            struct thread *t = list_entry(pos, struct thread, proc_link);
            if (t->tid == tid) {
                target = t;
                list_del(&t->proc_link);
                break;
            }
        }
        if (target)
            break;
        bool alive = false;
        list_for_each(pos, &p->threads) {
            if (list_entry(pos, struct thread, proc_link)->tid == tid)
                alive = true;
        }
        if (!alive || p->exiting) {
            spin_unlock(&p->lock);
            return alive ? -EINTR : -ESRCH;
        }
        /* The zombie list changes on every thread exit; poll by yielding
         * through the process wait queue. */
        spin_unlock(&p->lock);
        sched_yield();
        spin_lock(&p->lock);
    }
    spin_unlock(&p->lock);

    spin_lock(&target->exit_lock);
    while (!target->finished)
        waitq_wait(&target->exit_waitq, &target->exit_lock);
    spin_unlock(&target->exit_lock);
    int code = target->exit_code;
    thread_free(target);
    if (code_ptr)
        *(int *)code_ptr = code;
    return 0;
}
