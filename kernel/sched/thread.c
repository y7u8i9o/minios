#define KLOG_SUBSYS "thread"
#include <sched/thread.h>
#include <arch/thread.h>
#include <sched/proc.h>
#include <sched/sched.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <arch/cpu.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <debug/panic.h>

/* Thread ids. Protected by tid_lock. */
static DEFINE_SPINLOCK(tid_lock);
static int next_tid = 1;

void thread_start(void);

static int tid_alloc(void)
{
    spin_lock(&tid_lock);
    int tid = next_tid++;
    spin_unlock(&tid_lock);
    return tid;
}

/* First code run by a new thread, entered through its first switch
 * (arch_thread_init). The local run-queue lock is locked across the switch
 * and released here. */
void thread_start(void)
{
    struct thread *t = thread_current();
    arch_thread_resume(t);
    sched_finish_switch();
    sched_unlock_current();
    t->entry(t->arg);
    thread_exit(0);
}

struct thread *thread_alloc(struct proc *proc, const char *name, thread_fn fn, void *arg, int level)
{
    struct thread *t = kzalloc(sizeof *t);
    if (!t)
        return NULL;
    t->kstack_top = kstack_alloc();
    if (!t->kstack_top) {
        kfree(t);
        return NULL;
    }
    if (arch_thread_init(t, thread_start) < 0) {
        kstack_free(t->kstack_top);
        kfree(t);
        return NULL;
    }
    t->proc = proc;
    t->tid = tid_alloc();
    t->state = THREAD_NEW;
    t->level = level < 0 ? 0 : level >= MLFQ_LEVELS ? MLFQ_LEVELS - 1 : level;
    t->slice_left = 10 << t->level;
    t->cpu = cpu_current()->id;
    t->entry = fn;
    t->arg = arg;
    list_init(&t->run_link);
    spinlock_init(&t->exit_lock, "thread_exit");
    waitq_init(&t->exit_waitq, "thread_exit");
    strlcpy(t->name, name, sizeof t->name);

    spin_lock(&proc->lock);
    list_add_tail(&t->proc_link, &proc->threads);
    proc->nthreads++;
    spin_unlock(&proc->lock);
    return t;
}

struct thread *thread_create(const char *name, thread_fn fn, void *arg, int level)
{
    struct thread *t = thread_alloc(&kernel_proc, name, fn, arg, level);
    if (!t)
        return NULL;
    sched_add(t);
    return t;
}

struct thread *thread_current(void)
{
    return cpu_current()->current;
}

__noreturn void thread_exit(int code)
{
    struct thread *t = thread_current();
    kassert(t != NULL);
    struct proc *p = t->proc;

    t->exit_code = code;
    spin_lock(&p->lock);
    list_del(&t->proc_link);
    p->nthreads--;
    bool last = p->nthreads == 0;
    /* User threads are reaped with their process; kernel threads are
     * joined individually. The first thread to exit a process without an
     * explicit exit status supplies its own code. */
    if (p != &kernel_proc) {
        list_add_tail(&t->proc_link, &p->zombies);
        if (last && !__atomic_load_n(&p->exiting, __ATOMIC_RELAXED)) {
            __atomic_store_n(&p->exiting, true, __ATOMIC_RELEASE);
            p->exit_status = PROC_STATUS_EXITED(code);
        }
    }
    spin_unlock(&p->lock);
    if (last && p != &kernel_proc)
        proc_exit_notify(p);
    sched_lock_current();
    t->state = THREAD_ZOMBIE;
    /* Joiners are woken by the next thread once this stack is no longer
     * in use, see sched_switch_locked. */
    sched_switch_locked();
    panic("zombie thread %s resumed", t->name);
}

void thread_free(struct thread *t)
{
    kassert(t->state == THREAD_ZOMBIE);
    if (!t->on_boot_stack)
        kstack_free(t->kstack_top);
    arch_thread_free(t);
    kfree(t);
}

int thread_join(struct thread *t)
{
    spin_lock(&t->exit_lock);
    while (!t->finished)
        waitq_wait(&t->exit_waitq, &t->exit_lock);
    spin_unlock(&t->exit_lock);
    int code = t->exit_code;
    thread_free(t);
    return code;
}
