#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sync/spinlock.h>

struct spinlock;

/* A queue of blocked threads. lock protects waiters. Ordering:
 * caller's condition lock -> waitq.lock.  Runnable publication uses the
 * destination CPU's lock-free MPSC inbox. */
struct waitq {
    struct spinlock lock;
    struct list_head waiters;
};

#define WAITQ_INIT(name) { .lock = SPINLOCK_INIT(#name), .waiters = LIST_HEAD_INIT((name).waiters) }
#define DEFINE_WAITQ(name) struct waitq name = WAITQ_INIT(name)

void waitq_init(struct waitq *wq, const char *name);
/* Block the current thread until woken. held is the spinlock protecting
 * the condition; it is released while blocked and re-acquired before
 * return. May be NULL. Never call from interrupt context. */
void waitq_wait(struct waitq *wq, struct spinlock *held);
/* Wake one or all waiters. Returns the number of threads woken. */
int waitq_wake_one(struct waitq *wq);
int waitq_wake_all(struct waitq *wq);
/* Remove t from whatever queue it blocks on and make it runnable. The
 * woken waitq_wait returns as if woken normally; callers re-check their
 * condition and the process exit flag. */
struct thread;
void waitq_interrupt(struct thread *t);
/* Like waitq_wait, but the wait ends at deadline_ms (timer_ms clock)
 * as if woken; callers re-check their condition and the time. */
void waitq_wait_timeout(struct waitq *wq, struct spinlock *held, uint64_t deadline_ms);
/* Called from the timer interrupt on the boot CPU. */
void waitq_timeouts_tick(void);
