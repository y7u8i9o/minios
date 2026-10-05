#pragma once
#include <kernel.h>
#include <sync/spinlock.h>

struct thread;

/* Turn the boot context into the idle thread and start the kinit thread. */
void sched_init(void);
/* Turn the calling application processor's startup context into its idle
 * thread. */
void sched_init_cpu(void);
__noreturn void sched_idle_loop(void);

/* Give up the CPU, remaining runnable. */
void sched_yield(void);
/* Lock the calling CPU's run queue around a state change and switch. */
void sched_lock_current(void);
void sched_unlock_current(void);
/* Switch away with the local run-queue lock acquired and current state already
 * changed from RUNNING. Returns with the same local lock acquired. */
void sched_switch_locked(void);
/* Make a blocked or sleeping thread runnable. */
void sched_wake(struct thread *t);
/* Enqueue a fresh thread. */
void sched_add(struct thread *t);
/* The timer tick of the boot CPU and of an application processor.  Both
 * consume the inbound wakes of the local run queue, wake its sleepers whose
 * tick has come, account the slice of the running thread and perform the
 * priority boost of that CPU. */
void sched_tick(void);
void sched_tick_cpu(void);
/* sched_sleep_until sleeps on this CPU's sleeper list until the tick or a
 * wake.  interrupted, when not NULL, is called after the sleep is
 * published.  When it returns true, the function returns false without
 * sleeping.  A wake can end the sleep before the tick; the caller checks
 * the time (sleep_ms). */
bool sched_sleep_until(uint64_t tick, bool (*interrupted)(void));
/* True if a reschedule was requested by the timer. */
bool sched_need_resched(void);
/* Preemption point used before returning to user mode and by the idle loop. */
void sched_preempt(void);
bool sched_started(void);
/* Post switch bookkeeping, run with the local run-queue lock acquired. */
void sched_finish_switch(void);
void sched_dump(void);
/* sched_quiet reports whether every CPU runs its idle thread or the thread
 * self, with no ready thread, no pending wakeup in its inbox and no thread
 * of a user process that sleeps until a tick at or before until_tick.  The
 * function takes each run-queue lock with spin_try_lock, one at a time,
 * and a lock that another CPU has taken counts as activity.  The boot
 * tests use the result to wait until the system has processed their input
 * (ktest_wait_idle). */
bool sched_quiet(const struct thread *self, uint64_t until_tick);
