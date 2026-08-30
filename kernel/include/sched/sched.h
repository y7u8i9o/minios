#pragma once
#include <kernel.h>
#include <sync/spinlock.h>

struct thread;

/* Protects the run queues, the sleep list and every thread's scheduling
 * fields. Taken from the timer interrupt. */
extern struct spinlock sched_lock;

/* Turn the boot context into the idle thread and start the kinit thread. */
void sched_init(void);
/* Turn the calling application processor's startup context into its idle
 * thread. */
void sched_init_cpu(void);
__noreturn void sched_idle_loop(void);

/* Give up the CPU, staying runnable. */
void sched_yield(void);
/* Switch away with sched_lock held and the current thread's state already
 * set to something other than RUNNING. Returns with sched_lock held. */
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
/* Post switch bookkeeping, run on the resumed thread's stack with
 * sched_lock held. Also called by thread_start for brand new threads. */
void sched_finish_switch(void);
void sched_dump(void);
