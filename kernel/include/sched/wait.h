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
/* Block the current thread until woken. lock is the spinlock protecting
 * the condition; it is released while blocked and re-acquired before
 * return. May be NULL. Never call from interrupt context. */
void waitq_wait(struct waitq *wq, struct spinlock *lock);
/* Wake one or all waiters. Returns the number of threads woken. */
int waitq_wake_one(struct waitq *wq);
int waitq_wake_all(struct waitq *wq);
/* Remove t from whatever queue it blocks on and make it runnable. The
 * woken waitq_wait returns as if woken normally; callers re-check their
 * condition and the process exit flag. */
/* waitq_wait for a wait that ends soon in a working system: a sleeping
 * lock or the completion of a block request. The hung task detector
 * (debug/hung.c) reports a thread that waits here longer than its limit. */
void waitq_wait_bounded(struct waitq *wq, struct spinlock *lock);
struct thread;
void waitq_interrupt(struct thread *t);
/* waitq_interrupt for a signal or an exit sent to t. If t is about to call
 * waitq_wait and is not registered yet, its next waitq_wait returns at
 * once, so the caller checks for signals again. */
void waitq_signal(struct thread *t);
/* Like waitq_wait, but the wait ends at deadline_ms (timer_ms clock)
 * as if woken; callers re-check their condition and the time. */
void waitq_wait_timeout(struct waitq *wq, struct spinlock *lock, uint64_t deadline_ms);
/* waitq_next_user_deadline returns the earliest deadline (timer_ms clock)
 * of a thread of a user process in waitq_wait_timeout, or UINT64_MAX.  The
 * boot tests use it (ktest_wait_idle). */
uint64_t waitq_next_user_deadline(void);
/* Called from the timer interrupt on the boot CPU. */
void waitq_timeouts_tick(void);
