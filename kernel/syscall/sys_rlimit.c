/* Resource limits and usage (M40): getrlimit, setrlimit, prlimit, getrusage. */
#include <syscall/syscalls.h>
#include <syscall/rlimit.h>
#include <arch/trap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <fs/fdtable.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <errno.h>

static void ticks_to_timeval(uint64_t ticks, struct abi_timeval *tv)
{
    uint64_t us = ticks * (1000000 / TIMER_HZ);
    tv->tv_sec = (int64_t)(us / 1000000);
    tv->tv_usec = (int64_t)(us % 1000000);
}

void rusage_of_proc(struct proc *p, struct rusage *ru, bool children)
{
    memset(ru, 0, sizeof *ru);
    spin_lock(&p->lock);
    if (children) {
        ticks_to_timeval(p->cutime, &ru->ru_utime);
        ticks_to_timeval(p->cstime, &ru->ru_stime);
        ru->ru_minflt = (int64_t)p->cminflt;
        ru->ru_majflt = (int64_t)p->cmajflt;
        ru->ru_nvcsw = (int64_t)p->cnvcsw;
        ru->ru_nivcsw = (int64_t)p->cnivcsw;
    } else {
        ticks_to_timeval(__atomic_load_n(&p->utime, __ATOMIC_RELAXED), &ru->ru_utime);
        ticks_to_timeval(__atomic_load_n(&p->stime, __ATOMIC_RELAXED), &ru->ru_stime);
        ru->ru_minflt = (int64_t)__atomic_load_n(&p->minflt, __ATOMIC_RELAXED);
        ru->ru_majflt = (int64_t)__atomic_load_n(&p->majflt, __ATOMIC_RELAXED);
        ru->ru_nvcsw = (int64_t)__atomic_load_n(&p->nvcsw, __ATOMIC_RELAXED);
        ru->ru_nivcsw = (int64_t)__atomic_load_n(&p->nivcsw, __ATOMIC_RELAXED);
    }
    spin_unlock(&p->lock);
    if (!children && p->vm && p->state != PROC_ZOMBIE)
        ru->ru_maxrss = (int64_t)(vma_count_resident(p->vm) * (PAGE_SIZE / 1024));
}

/* Apply a new limit to p. Called with no lock held. */
static long set_limit(struct proc *p, int resource, const struct rlimit *new)
{
    if (new->rlim_cur > new->rlim_max)
        return -EINVAL;
    if (resource == RLIMIT_NOFILE && new->rlim_max > OPEN_MAX)
        return -EPERM;
    spin_lock(&p->lock);
    p->rlim[resource] = *new;
    spin_unlock(&p->lock);
    if (resource == RLIMIT_NOFILE)
        fdtable_set_limit(&p->fds, (int)MIN(new->rlim_cur, (uint64_t)OPEN_MAX));
    return 0;
}

/* prlimit(pid, resource, new, old): the core of the three limit calls.
 * pid 0 names the caller. */
static long do_prlimit(int pid, int resource, uintptr_t new_ptr, uintptr_t old_ptr)
{
    if (resource < 0 || resource >= RLIMIT_NLIMITS)
        return -EINVAL;
    if (new_ptr && !user_range_ok(new_ptr, sizeof(struct rlimit), false))
        return -EFAULT;
    if (old_ptr && !user_range_ok(old_ptr, sizeof(struct rlimit), true))
        return -EFAULT;
    struct proc *p = thread_current()->proc;
    if (pid != 0 && pid != p->pid) {
        p = proc_find(pid);
        if (!p || p == &kernel_proc)
            return -ESRCH;
    }
    struct rlimit new;
    if (new_ptr)
        memcpy(&new, (const void *)new_ptr, sizeof new);
    struct rlimit old;
    spin_lock(&p->lock);
    old = p->rlim[resource];
    spin_unlock(&p->lock);
    if (new_ptr) {
        long r = set_limit(p, resource, &new);
        if (r < 0)
            return r;
    }
    if (old_ptr)
        memcpy((void *)old_ptr, &old, sizeof old);
    return 0;
}

long sys_getrlimit(struct trapframe *tf)
{
    return do_prlimit(0, (int)SYSARG0(tf), 0, SYSARG1(tf));
}

long sys_setrlimit(struct trapframe *tf)
{
    if (!SYSARG1(tf))
        return -EFAULT;
    return do_prlimit(0, (int)SYSARG0(tf), SYSARG1(tf), 0);
}

long sys_prlimit(struct trapframe *tf)
{
    return do_prlimit((int)SYSARG0(tf), (int)SYSARG1(tf), SYSARG2(tf), SYSARG3(tf));
}

long sys_getrusage(struct trapframe *tf)
{
    int who = (int)SYSARG0(tf);
    uintptr_t ptr = SYSARG1(tf);
    if (!ptr || !user_range_ok(ptr, sizeof(struct rusage), true))
        return -EFAULT;
    struct thread *t = thread_current();
    struct rusage ru;
    if (who == RUSAGE_SELF) {
        rusage_of_proc(t->proc, &ru, false);
    } else if (who == RUSAGE_CHILDREN) {
        rusage_of_proc(t->proc, &ru, true);
    } else if (who == RUSAGE_THREAD) {
        memset(&ru, 0, sizeof ru);
        ticks_to_timeval(t->utime, &ru.ru_utime);
        ticks_to_timeval(t->stime, &ru.ru_stime);
        ru.ru_nvcsw = (int64_t)t->nvcsw;
        ru.ru_nivcsw = (int64_t)t->nivcsw;
    } else {
        return -EINVAL;
    }
    memcpy((void *)ptr, &ru, sizeof ru);
    return 0;
}
