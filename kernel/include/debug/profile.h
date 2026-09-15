#pragma once
#include <kernel.h>
#include <minios/abi.h>

struct trapframe;
struct thread;

/* Full system profiler (M48). One lock free ring per CPU collects typed
 * events: timer samples, scheduler transitions, kernel heap activity and
 * completed transfers. /dev/profile hands them to user space in time
 * order. See docs/design/profile.md. */

void profile_init(void);

/* The recording mask: a set bit means events of that type are wanted.
 * Zero while the profiler is stopped, so every hook costs one relaxed
 * load and a test on its subsystem's fast path. */
extern uint32_t prof_event_mask;
static inline bool profile_wants(unsigned type)
{
    return __atomic_load_n(&prof_event_mask, __ATOMIC_RELAXED) & PROF_MASK(type);
}

/* Called from the timer interrupt of every CPU with interrupts disabled. */
void profile_sample(const struct trapframe *tf);
/* Called from the timer interrupt after the sample: publishes readiness to
 * pollers, which the recorders cannot do because they run under locks. */
void profile_tick(void);

/* Scheduler hooks, called from sched_switch_locked with the run queue lock
 * held. They take no lock and never wake a poller. */
void profile_leave_cpu(struct thread *prev, bool preempted);
void profile_enter_cpu(struct thread *next);
/* Called when a thread becomes runnable, to measure run queue latency. */
void profile_ready(struct thread *t);

/* Kernel heap hooks. size is the requested size on allocation and the
 * object size, or zero when it is not known, on release. */
void profile_heap(bool releasing, const void *addr, size_t size);

/* Completed transfer. start_ns comes from timer_ns() before the transfer. */
void profile_io(bool write, bool block_device, uint64_t bytes, uint64_t start_ns);

/* Unwinding (debug/unwind.c). Fills chain with up to max addresses,
 * innermost first, and returns the depth. tf describes an interrupted
 * frame; when it is NULL the walk starts at the caller of the recorder,
 * whose frame pointer is rbp. flags collects PROF_FLAG_USER, KUSER and
 * TRUNC. */
unsigned prof_unwind(const struct trapframe *tf, uintptr_t rbp, struct thread *t,
                     uint64_t *chain, unsigned max, uint8_t *flags);
