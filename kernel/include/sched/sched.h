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
/* Timer tick hook of the boot CPU: sleepers, boost and local accounting. */
void sched_tick(void);
/* Timer tick of an application processor: local slice accounting only. */
void sched_tick_cpu(void);
/* Block the current thread until the given tick. */
void sched_sleep_until(uint64_t tick);
/* True if a reschedule was requested by the timer. */
bool sched_need_resched(void);
/* Preemption point used before returning to user mode and by the idle loop. */
void sched_preempt(void);
bool sched_started(void);
/* Post switch bookkeeping, run with the local run-queue lock acquired. */
void sched_finish_switch(void);
void sched_dump(void);
