#define KLOG_SUBSYS "proc"
#include <sched/proc.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>
#include <debug/panic.h>
#include <lib/printf.h>
#include <ipc/signal.h>
#include <arch/frame.h>
#include <arch/cpu.h>
#include <drivers/timer.h>

/* All processes and the pid counter. Protected by proc_list_lock. */
static LIST_HEAD(proc_list);
static DEFINE_SPINLOCK(proc_list_lock);
static int next_pid = 1;

struct spinlock proc_tree_lock = SPINLOCK_INIT("proc_tree_lock");
struct proc kernel_proc;
static struct proc *init_proc;

static void proc_setup(struct proc *p, int pid, const char *name, struct proc *parent)
{
    memset(p, 0, sizeof *p);
    p->pid = pid;
    list_init(&p->threads);
    list_init(&p->zombies);
    list_init(&p->children);
    list_init(&p->sibling);
    p->state = PROC_RUNNING;
    p->parent = parent;
    p->pgid = pid;
    spinlock_init(&p->lock, "proc");
    waitq_init(&p->exit_waitq, "proc_exit");
    waitq_init(&p->child_waitq, "proc_child");
    strlcpy(p->name, name, sizeof p->name);
    strlcpy(p->cwd, parent ? parent->cwd : "/", sizeof p->cwd);
    fdtable_init(&p->fds);
    if (parent) {
        spin_lock(&parent->lock);
        memcpy(p->rlim, parent->rlim, sizeof p->rlim);
        spin_unlock(&parent->lock);
    } else {
        for (int i = 0; i < RLIMIT_NLIMITS; i++)
            p->rlim[i].rlim_cur = p->rlim[i].rlim_max = RLIM_INFINITY;
        p->rlim[RLIMIT_NOFILE].rlim_cur = p->rlim[RLIMIT_NOFILE].rlim_max = OPEN_MAX;
        p->rlim[RLIMIT_STACK].rlim_cur = 8UL << 20;
    }
    fdtable_set_limit(&p->fds, (int)MIN(p->rlim[RLIMIT_NOFILE].rlim_cur, (uint64_t)OPEN_MAX));
    spin_lock(&proc_list_lock);
    list_add_tail(&p->link, &proc_list);
    spin_unlock(&proc_list_lock);
    if (parent) {
        spin_lock(&proc_tree_lock);
        list_add_tail(&p->sibling, &parent->children);
        spin_unlock(&proc_tree_lock);
    }
}

void proc_init(void)
{
    proc_setup(&kernel_proc, 0, "kernel", NULL);
}

void proc_set_init(struct proc *p)
{
    init_proc = p;
}

struct proc *proc_alloc(const char *name, struct proc *parent)
{
    struct proc *p = kmalloc(sizeof *p);
    if (!p)
        return NULL;
    spin_lock(&proc_list_lock);
    int pid = next_pid++;
    spin_unlock(&proc_list_lock);
    proc_setup(p, pid, name, parent);
    return p;
}

struct proc *proc_find(int pid)
{
    struct list_head *pos;
    struct proc *found = NULL;
    spin_lock(&proc_list_lock);
    list_for_each(pos, &proc_list) {
        struct proc *p = list_entry(pos, struct proc, link);
        if (p->pid == pid) {
            found = p;
            break;
        }
    }
    spin_unlock(&proc_list_lock);
    return found;
}

void proc_free(struct proc *p)
{
    kassert(p != &kernel_proc);
    kassert(p->nthreads == 0);
    spin_lock(&proc_list_lock);
    list_del(&p->link);
    spin_unlock(&proc_list_lock);
    /* A child whose fork failed before it ran is still in its parent's
     * children list; leaving it there would make the parent wait forever
     * on freed memory. Reaped processes are already unlinked. */
    spin_lock(&proc_tree_lock);
    if (!list_empty(&p->sibling))
        list_del(&p->sibling);
    spin_unlock(&proc_tree_lock);
    fdtable_close_all(&p->fds);
    if (p->vm) {
        vma_remove_all(p->vm);
        vmspace_destroy(p->vm);
    }
    kfree(p);
}

void proc_begin_exit(struct proc *p, int status)
{
    spin_lock(&p->lock);
    if (!__atomic_load_n(&p->exiting, __ATOMIC_RELAXED)) {
        __atomic_store_n(&p->exiting, true, __ATOMIC_RELEASE);
        p->exit_status = status;
    }
    /* Kick threads blocked in the kernel so they notice. */
    struct list_head *pos;
    list_for_each(pos, &p->threads) {
        struct thread *t = list_entry(pos, struct thread, proc_link);
        if (t != thread_current()) {
            waitq_signal(t);
            sched_wake(t);          /* SIGKILL must also release stopped threads */
        }
    }
    spin_unlock(&p->lock);
}

void proc_exit_notify(struct proc *p)
{
    /* Release open files first so pipe peers see the end promptly. */
    fdtable_close_all(&p->fds);
    spin_lock(&proc_tree_lock);
    p->state = PROC_ZOMBIE;
    /* Orphans go to init. */
    while (!list_empty(&p->children)) {
        struct proc *c = list_first_entry(&p->children, struct proc, sibling);
        list_del(&c->sibling);
        struct proc *target = init_proc && init_proc != p ? init_proc : &kernel_proc;
        c->parent = target;
        list_add_tail(&c->sibling, &target->children);
        if (c->state == PROC_ZOMBIE)
            waitq_wake_all(&target->child_waitq);
    }
    struct proc *parent = p->parent;
    spin_unlock(&proc_tree_lock);
    waitq_wake_all(&p->exit_waitq);
    if (parent) {
        waitq_wake_all(&parent->child_waitq);
        if (parent != &kernel_proc)
            signal_send(parent, SIGCHLD);
    }
}

void proc_reap_children(struct proc *parent)
{
    for (;;) {
        spin_lock(&proc_tree_lock);
        struct proc *child = list_empty(&parent->children) ? NULL
            : list_first_entry(&parent->children, struct proc, sibling);
        spin_unlock(&proc_tree_lock);
        if (!child)
            return;
        proc_reap(child);
    }
}

int proc_reap(struct proc *p)
{
    spin_lock(&proc_tree_lock);
    while (p->state != PROC_ZOMBIE)
        waitq_wait(&p->exit_waitq, &proc_tree_lock);
    if (!list_empty(&p->sibling)) {
        list_del(&p->sibling);
        list_init(&p->sibling);
    }
    spin_unlock(&proc_tree_lock);

    /* Every thread has exited; wait until each has switched away. */
    spin_lock(&p->lock);
    int status = p->exit_status;
    while (!list_empty(&p->zombies)) {
        struct thread *t = list_first_entry(&p->zombies, struct thread, proc_link);
        list_del(&t->proc_link);
        spin_unlock(&p->lock);
        spin_lock(&t->exit_lock);
        while (!t->finished)
            waitq_wait(&t->exit_waitq, &t->exit_lock);
        spin_unlock(&t->exit_lock);
        thread_free(t);
        spin_lock(&p->lock);
    }
    spin_unlock(&p->lock);
    /* The child's consumption joins the reaper's children totals. */
    struct proc *self = thread_current()->proc;
    spin_lock(&self->lock);
    self->cutime += p->utime + p->cutime;
    self->cstime += p->stime + p->cstime;
    self->cminflt += p->minflt + p->cminflt;
    self->cmajflt += p->majflt + p->cmajflt;
    self->cnvcsw += p->nvcsw + p->cnvcsw;
    self->cnivcsw += p->nivcsw + p->cnivcsw;
    spin_unlock(&self->lock);
    proc_free(p);
    return status;
}

int proc_count_users(void)
{
    int n = 0;
    spin_lock(&proc_tree_lock);
    spin_lock(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list) {
        struct proc *p = list_entry(pos, struct proc, link);
        if (p != &kernel_proc && p->state != PROC_ZOMBIE)
            n++;
    }
    spin_unlock(&proc_list_lock);
    spin_unlock(&proc_tree_lock);
    return n;
}

uint64_t proc_rlimit_cur(struct proc *p, int resource)
{
    return __atomic_load_n(&p->rlim[resource].rlim_cur, __ATOMIC_RELAXED);
}

void proc_account_tick(const struct trapframe *tf)
{
    struct cpu *c = cpu_current();
    struct thread *t = c->current;
    if (!t || t == c->idle)
        return;
    struct proc *p = t->proc;
    bool user = frame_from_user(tf);
    if (user) {
        t->utime++;
        __atomic_fetch_add(&p->utime, 1, __ATOMIC_RELAXED);
    } else {
        t->stime++;
        __atomic_fetch_add(&p->stime, 1, __ATOMIC_RELAXED);
    }
    if (p == &kernel_proc)
        return;
    uint64_t soft = proc_rlimit_cur(p, RLIMIT_CPU);
    uint64_t hard = __atomic_load_n(&p->rlim[RLIMIT_CPU].rlim_max, __ATOMIC_RELAXED);
    if (soft == RLIM_INFINITY && hard == RLIM_INFINITY)
        return;
    uint64_t total = __atomic_load_n(&p->utime, __ATOMIC_RELAXED) + __atomic_load_n(&p->stime, __ATOMIC_RELAXED);
    if (hard != RLIM_INFINITY && total >= hard * TIMER_HZ) {
        signal_send(p, SIGKILL);
        return;
    }
    /* SIGXCPU when the soft limit is reached and once per second after. */
    if (soft != RLIM_INFINITY && total >= soft * TIMER_HZ && (total - soft * TIMER_HZ) % TIMER_HZ == 0)
        signal_send(p, SIGXCPU);
}

int proc_count_others(void)
{
    int n = 0;
    spin_lock(&proc_tree_lock);
    spin_lock(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list) {
        struct proc *p = list_entry(pos, struct proc, link);
        if (p != &kernel_proc && p != init_proc && p->state != PROC_ZOMBIE)
            n++;
    }
    spin_unlock(&proc_list_lock);
    spin_unlock(&proc_tree_lock);
    return n;
}

void proc_exit_check(void)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    if (p == &kernel_proc)
        return;
    if (__atomic_load_n(&p->exiting, __ATOMIC_ACQUIRE))
        thread_exit(0);
}

int proc_collect_pgrp(int pgid, int *pids, int max)
{
    int n = 0;
    spin_lock(&proc_tree_lock);
    spin_lock(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list) {
        struct proc *p = list_entry(pos, struct proc, link);
        if (p == &kernel_proc || p->state == PROC_ZOMBIE)
            continue;
        if (pgid == 0 ? p == init_proc : p->pgid != pgid)
            continue;
        if (n < max)
            pids[n++] = p->pid;
    }
    spin_unlock(&proc_list_lock);
    spin_unlock(&proc_tree_lock);
    return n;
}

/* One row of /dev/proc, copied out of the process under the locks. */
struct proc_row {
    int pid, ppid, pgid;
    bool zombie, stopped;
    uint64_t ticks;
    struct vmspace *vm;
    char name[PROC_NAME_LEN];
};

size_t proc_format_maps(char *buf, size_t size)
{
    size_t off = 0;
    off += (size_t)ksnprintf(buf + off, size - off, "%5s %16s %16s %10s %s\n", "PID", "START", "END", "OFFSET", "PATH");
    enum { MAX_ROWS = 64 };
    static int pids[MAX_ROWS];      /* the device read is serialized by the file lock */
    int n = 0;
    spin_lock(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list) {
        struct proc *p = list_entry(pos, struct proc, link);
        if (n == MAX_ROWS)
            break;
        pids[n++] = p->pid;
    }
    spin_unlock(&proc_list_lock);
    for (int i = 0; i < n && off < size - 1; i++) {
        /* proc_tree_lock keeps the process from being reaped and sits above
         * vmspace.lock in the lock order. */
        spin_lock(&proc_tree_lock);
        struct proc *p = proc_find(pids[i]);
        if (p && p->state != PROC_ZOMBIE && p->vm) {
            struct vmspace *vm = p->vm;
            spin_lock(&vm->lock);
            struct list_head *vpos;
            list_for_each(vpos, &vm->vmas) {
                struct vma *v = list_entry(vpos, struct vma, link);
                if (!(v->flags & VM_FILE) || !v->file || !v->file->path || off >= size - 1)
                    continue;
                off += (size_t)ksnprintf(buf + off, size - off, "%5d %16lx %16lx %10lx %s\n", p->pid,
                                         (unsigned long)v->start, (unsigned long)v->end,
                                         (unsigned long)v->offset, v->file->path);
            }
            spin_unlock(&vm->lock);
        }
        spin_unlock(&proc_tree_lock);
    }
    return off < size ? off : size - 1;
}

size_t proc_format_table(char *buf, size_t size)
{
    size_t off = 0;
    off += (size_t)ksnprintf(buf + off, size - off, "%5s %5s %5s %-8s %8s %8s %s\n", "PID", "PPID", "PGID", "STATE", "TIME", "RSS", "NAME");
    /* Snapshot first: counting resident pages walks page tables and must
     * not run under the process locks. The rows hold pids only, so a
     * process that exits meanwhile is reported without its size. */
    enum { MAX_ROWS = 64 };
    static struct proc_row rows[MAX_ROWS];  /* procdev_read is serialized by the file lock */
    int n = 0;
    spin_lock(&proc_tree_lock);
    spin_lock(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list) {
        struct proc *p = list_entry(pos, struct proc, link);
        if (n == MAX_ROWS)
            break;
        struct proc_row *r = &rows[n++];
        r->pid = p->pid;
        r->ppid = p->parent ? p->parent->pid : 0;
        r->pgid = p->pgid;
        r->zombie = p->state == PROC_ZOMBIE;
        r->stopped = p->stopped;
        r->ticks = __atomic_load_n(&p->utime, __ATOMIC_RELAXED) + __atomic_load_n(&p->stime, __ATOMIC_RELAXED);
        r->vm = r->zombie ? NULL : p->vm;
        memcpy(r->name, p->name, sizeof r->name);
    }
    spin_unlock(&proc_list_lock);
    spin_unlock(&proc_tree_lock);
    for (int i = 0; i < n && off < size - 1; i++) {
        struct proc_row *r = &rows[i];
        /* The process may have been reaped meanwhile. proc_tree_lock keeps
         * it from being reaped during the walk (proc_reap takes it) and
         * sits above vmspace.lock in the lock order; proc_list_lock, a
         * leaf, is not held while the page tables are walked. */
        size_t rss = 0;
        if (r->vm) {
            spin_lock(&proc_tree_lock);
            struct proc *p = proc_find(r->pid);
            if (p && p->state != PROC_ZOMBIE && p->vm == r->vm)
                rss = vma_count_resident(r->vm) * (PAGE_SIZE / 1024);
            spin_unlock(&proc_tree_lock);
        }
        off += (size_t)ksnprintf(buf + off, size - off, "%5d %5d %5d %-8s %8lu %8zu %s\n", r->pid, r->ppid, r->pgid,
                                 r->zombie ? "zombie" : r->stopped ? "stopped" : "running",
                                 r->ticks, rss, r->name);
    }
    return off < size ? off : size - 1;
}
