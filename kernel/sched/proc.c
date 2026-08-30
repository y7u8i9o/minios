#define KLOG_SUBSYS "proc"
#include <sched/proc.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>
#include <debug/panic.h>
#include <lib/printf.h>

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
    if (!p->exiting) {
        p->exiting = true;
        p->exit_status = status;
    }
    /* Kick threads blocked in the kernel so they notice. */
    struct list_head *pos;
    list_for_each(pos, &p->threads) {
        struct thread *t = list_entry(pos, struct thread, proc_link);
        if (t != thread_current())
            waitq_interrupt(t);
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

int proc_reap(struct proc *p)
{
    spin_lock(&proc_tree_lock);
    while (p->state != PROC_ZOMBIE)
        waitq_wait(&p->exit_waitq, &proc_tree_lock);
    if (!list_empty(&p->sibling))
        list_del(&p->sibling);
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
    proc_free(p);
    return status;
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
    spin_lock(&p->lock);
    bool exiting = p->exiting;
    spin_unlock(&p->lock);
    if (exiting)
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

size_t proc_format_table(char *buf, size_t size)
{
    size_t off = 0;
    off += (size_t)ksnprintf(buf + off, size - off, "%5s %5s %5s %-8s %s\n", "PID", "PPID", "PGID", "STATE", "NAME");
    spin_lock(&proc_tree_lock);
    spin_lock(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list) {
        struct proc *p = list_entry(pos, struct proc, link);
        if (off >= size - 1)
            break;
        off += (size_t)ksnprintf(buf + off, size - off, "%5d %5d %5d %-8s %s\n", p->pid,
                                 p->parent ? p->parent->pid : 0, p->pgid,
                                 p->state == PROC_ZOMBIE ? "zombie" : "running", p->name);
    }
    spin_unlock(&proc_list_lock);
    spin_unlock(&proc_tree_lock);
    return off < size ? off : size - 1;
}
