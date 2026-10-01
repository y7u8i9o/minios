#include <sched/wait.h>
#include <drivers/timer.h>
#include <lib/list.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <kassert.h>
#include <sync/rcu.h>
#include <debug/panic.h>
#include <arch/cpu.h>

void waitq_init(struct waitq *wq, const char *name)
{
    spinlock_init(&wq->lock, name);
    list_init(&wq->waiters);
}

void waitq_wait(struct waitq *wq, struct spinlock *held)
{
    struct thread *t = thread_current();
    kassert(t != NULL);
    kassert(t != cpu_current()->idle);
    if (rcu_read_held())
        panic("sleep inside RCU read section");

    /* Register as a waiter and mark blocked before releasing the condition
     * lock, so a wakeup between release and switch cannot be lost. */
    spin_lock(&wq->lock);
    list_add_tail(&t->run_link, &wq->waiters);
    __atomic_store_n(&t->waiting_on, wq, __ATOMIC_SEQ_CST);
    /* A signal sent after the caller checked for signals and before
     * waiting_on was stored found no queue to interrupt. waitq_signal
     * stores sig_wake before it reads waiting_on, and both sides are
     * sequentially consistent, so either the sender finds the queue or
     * this exchange finds the flag. The wait then ends at once, and a
     * caller that does not check signals waits again with the flag clear. */
    if (__atomic_exchange_n(&t->sig_wake, false, __ATOMIC_SEQ_CST)) {
        list_del(&t->run_link);
        __atomic_store_n(&t->waiting_on, NULL, __ATOMIC_SEQ_CST);
        spin_unlock(&wq->lock);
        return;
    }
    sched_lock_current();
    /* Release: a drain that reads BLOCKED also reads waiting_on (mlfq.c,
     * drain_inbound_locked). */
    __atomic_store_n(&t->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
    spin_unlock(&wq->lock);
    if (held)
        spin_unlock(held);
    sched_switch_locked();
    sched_unlock_current();
    if (held)
        spin_lock(held);
}

static int wake(struct waitq *wq, bool all)
{
    int n = 0;
    spin_lock(&wq->lock);
    while (!list_empty(&wq->waiters)) {
        struct thread *t = list_first_entry(&wq->waiters, struct thread, run_link);
        if (t->waiting_on != wq || !t->run_link.prev || !t->run_link.next)
            panic("bad waiter wq %p next %p prev %p thread %p tid %d state %d waiting %p", wq,
                  wq->waiters.next, wq->waiters.prev, t, t->tid, t->state, t->waiting_on);
        list_del(&t->run_link);
        /* Sequentially consistent, before the claim of queue_inbound: a
         * drain that finds the claim released must not read the old
         * waiting_on and discard this wake as stale (A8). */
        __atomic_store_n(&t->waiting_on, NULL, __ATOMIC_SEQ_CST);
        sched_wake(t);
        n++;
        if (!all)
            break;
    }
    spin_unlock(&wq->lock);
    return n;
}

int waitq_wake_one(struct waitq *wq)
{
    return wake(wq, false);
}

int waitq_wake_all(struct waitq *wq)
{
    return wake(wq, true);
}

void waitq_interrupt(struct thread *t)
{
    struct waitq *wq = __atomic_load_n(&t->waiting_on, __ATOMIC_SEQ_CST);
    if (!wq)
        return;
    spin_lock(&wq->lock);
    if (t->waiting_on == wq) {
        list_del(&t->run_link);
        __atomic_store_n(&t->waiting_on, NULL, __ATOMIC_SEQ_CST);  /* see wake */
        sched_wake(t);
    }
    spin_unlock(&wq->lock);
}

void waitq_signal(struct thread *t)
{
    __atomic_store_n(&t->sig_wake, true, __ATOMIC_SEQ_CST);
    waitq_interrupt(t);
}

/* ---- timed waits (M23) ---- */

/* Waiters with a deadline. timed_lock protects the list and is taken
 * from the timer interrupt. Order: the caller's condition lock ->
 * timed_lock, and timed_lock -> wq->lock inside waitq_interrupt. */
struct timed_waiter {
    struct list_head link;
    struct thread *t;
    uint64_t deadline_ms;
};

static LIST_HEAD(timed_waiters);
static DEFINE_SPINLOCK(timed_lock);

void waitq_wait_timeout(struct waitq *wq, struct spinlock *held, uint64_t deadline_ms)
{
    struct timed_waiter w = { .t = thread_current(), .deadline_ms = deadline_ms };
    spin_lock(&timed_lock);
    list_add_tail(&w.link, &timed_waiters);
    spin_unlock(&timed_lock);
    waitq_wait(wq, held);
    spin_lock(&timed_lock);
    list_del(&w.link);
    spin_unlock(&timed_lock);
}

void waitq_timeouts_tick(void)
{
    uint64_t now = timer_ms();
    spin_lock(&timed_lock);
    struct list_head *pos;
    list_for_each(pos, &timed_waiters) {
        struct timed_waiter *w = list_entry(pos, struct timed_waiter, link);
        if (w->deadline_ms <= now)
            waitq_interrupt(w->t);
    }
    spin_unlock(&timed_lock);
}
