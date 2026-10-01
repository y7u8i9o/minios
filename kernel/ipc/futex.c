/* Futexes (M35): the blocking half of user space mutexes and condition
 * variables. A waiter hashes (process, address) to a bucket, links a
 * record with its own wait queue there and sleeps; a wake walks the
 * bucket and wakes the records of the same key. Spurious wakeups are
 * allowed and the callers re-check their word. */
#define KLOG_SUBSYS "futex"
#include <ipc/futex.h>
#include <ipc/signal.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <sched/wait.h>
#include <syscall/syscalls.h>
#include <drivers/timer.h>
#include <lib/list.h>
#include <errno.h>

#define FUTEX_BUCKETS 64

/* A sleeping thread. Lives on the waiter's kernel stack; linked into a
 * bucket while it may be woken, protected by the bucket's lock. */
struct futex_waiter {
    struct proc *proc;
    uintptr_t uaddr;
    struct waitq wq;
    struct list_head link;
    bool woken;
};

/* lock protects waiters and the woken flag of every record on it. */
struct futex_bucket {
    struct spinlock lock;
    struct list_head waiters;
};

static struct futex_bucket buckets[FUTEX_BUCKETS];

void futex_init(void)
{
    for (int i = 0; i < FUTEX_BUCKETS; i++) {
        spinlock_init(&buckets[i].lock, "futex_bucket");
        list_init(&buckets[i].waiters);
    }
}

static struct futex_bucket *bucket_of(struct proc *p, uintptr_t uaddr)
{
    uint64_t h = ((uint64_t)(uintptr_t)p >> 4) ^ (uaddr >> 2) ^ (uaddr >> 13);
    return &buckets[h % FUTEX_BUCKETS];
}

int futex_wait(uintptr_t uaddr, uint32_t value, uint64_t timeout_ms)
{
    struct thread *t = thread_current();
    struct futex_waiter w = { .proc = t->proc, .uaddr = uaddr, .woken = false };
    waitq_init(&w.wq, "futex");
    struct futex_bucket *b = bucket_of(t->proc, uaddr);
    uint64_t deadline = timeout_ms ? timer_ms() + timeout_ms : 0;

    spin_lock(&b->lock);
    /* The check of the word happens under the bucket lock so that a wake
     * after the caller's own check cannot be missed. */
    if (*(volatile uint32_t *)uaddr != value) {
        spin_unlock(&b->lock);
        return -EAGAIN;
    }
    list_add_tail(&w.link, &b->waiters);
    int r = 0;
    for (;;) {
        if (w.woken)
            break;
        if (signal_should_interrupt()) {
            r = -EINTR;
            break;
        }
        if (deadline) {
            if (timer_ms() >= deadline) {
                r = -ETIMEDOUT;
                break;
            }
            waitq_wait_timeout(&w.wq, &b->lock, deadline);
        } else {
            waitq_wait(&w.wq, &b->lock);
        }
    }
    if (!w.woken)
        list_del(&w.link);
    spin_unlock(&b->lock);
    return r;
}

int futex_wake(uintptr_t uaddr, uint32_t count)
{
    struct proc *p = thread_current()->proc;
    struct futex_bucket *b = bucket_of(p, uaddr);
    int woken = 0;
    spin_lock(&b->lock);
    struct list_head *pos, *next;
    list_for_each_safe(pos, next, &b->waiters) {
        struct futex_waiter *w = list_entry(pos, struct futex_waiter, link);
        if (w->proc != p || w->uaddr != uaddr)
            continue;
        list_del(&w->link);
        w->woken = true;
        waitq_wake_all(&w->wq);
        if (++woken >= (int)count)
            break;
    }
    spin_unlock(&b->lock);
    return woken;
}

long sys_futex(struct trapframe *tf)
{
    uintptr_t uaddr = SYSARG0(tf);
    uint32_t op = (uint32_t)SYSARG1(tf), value = (uint32_t)SYSARG2(tf);
    uint64_t timeout_ms = SYSARG3(tf);
    if (uaddr & 3 || !user_range_ok(uaddr, 4, false))
        return -EFAULT;
    switch (op) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, value, timeout_ms);
    case FUTEX_WAKE:
        return futex_wake(uaddr, value ? value : 1);
    default:
        return -EINVAL;
    }
}
