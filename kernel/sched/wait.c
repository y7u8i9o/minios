#include <sched/wait.h>
#include <drivers/timer.h>
#include <lib/list.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <kassert.h>

void waitq_init(struct waitq *wq, const char *name)
{
    spinlock_init(&wq->lock, name);
    list_init(&wq->waiters);
}

void waitq_wait(struct waitq *wq, struct spinlock *held)
{
    struct thread *t = thread_current();
    kassert(t != NULL);

    /* Register as a waiter and mark blocked before releasing the condition
     * lock, so a wakeup between the release and the switch is not lost:
     * the waker needs wq->lock to find us and sched_lock to change state. */
    spin_lock(&wq->lock);
    list_add_tail(&t->run_link, &wq->waiters);
    t->waiting_on = wq;
    spin_lock(&sched_lock);
    t->state = THREAD_BLOCKED;
    spin_unlock(&wq->lock);
    if (held)
        spin_unlock(held);
    sched_switch_locked();
    spin_unlock(&sched_lock);
    if (held)
        spin_lock(held);
}

static int wake(struct waitq *wq, bool all)
{
    int n = 0;
    spin_lock(&wq->lock);
    while (!list_empty(&wq->waiters)) {
        struct thread *t = list_first_entry(&wq->waiters, struct thread, run_link);
        list_del(&t->run_link);
        t->waiting_on = NULL;
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
        t->waiting_on = NULL;
        sched_wake(t);
    }
    spin_unlock(&wq->lock);
}

/* ---- timed waits (M23) ---- */

/* Waiters with a deadline; timed_lock protects the list and is taken
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
