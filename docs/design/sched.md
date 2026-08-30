# Kernel threads and scheduler (M7)

## Threads and processes

`struct thread` (`sched/thread.h`) holds the saved stack pointer, the kernel
stack, the owning process, state, MLFQ level, remaining slice, wake tick and
the list link used for run queues, the sleep list and wait queues. All
scheduling fields are protected by `sched_lock`. Kernel stacks come from
`kstack_alloc` (M4) and therefore have guard pages.

`struct proc` (`sched/proc.h`) owns an address space (NULL for the kernel
process), its thread list, state and exit status behind `proc.lock`.
Process 0 is `kernel_proc`, owner of every kernel thread.

## Context switch

`arch/x86_64/context.S` implements `context_switch(&old_sp, new_sp)`: push
the six callee saved registers, store `rsp`, load the new `rsp`, pop and
return. A new thread's stack is prepared by `thread_alloc` to look like such
a frame returning into `thread_start`, which runs the post switch step,
releases `sched_lock` and calls the entry function.

`sched_switch_locked` is the single switch point. It is entered with
`sched_lock` held and the current thread already marked ready, blocked,
sleeping or zombie. It picks the next thread, updates `cpu->current`, the
TSS `rsp0`, `cpu->kstack_top` and CR3 if the process differs, then switches.
The per CPU `int_enabled` value is saved across the switch, as in xv6, so
each thread's interrupt state is restored correctly by `pop_cli`.

A thread that exits cannot free its own stack. The scheduler records it in
`zombie_pending` and the next thread to run, whether resumed from
`context_switch` or started fresh through `thread_start`, calls
`sched_finish_switch`, which sets the zombie's `finished` flag and wakes
its joiners. `sched_lock` is released around that wakeup to keep the
`exit_lock -> waitq.lock -> sched_lock` order. `thread_join` waits on
`finished` under `exit_lock` and then frees the thread.

## MLFQ

Eight queues. Level 0 has a 10 ms slice; each lower level doubles it. The
timer tick decrements the running thread's slice and requests a reschedule
when it reaches zero, which is honoured at the next preemption point:
`sched_yield`, the idle loop, or (from M8) the return to user mode. The
kernel itself is never preempted. `sched_yield` demotes a thread whose slice
is exhausted. Waking from a wait queue or from sleep promotes by one level.
Every second `boost_locked` moves every ready thread back to level 0.

The idle thread is the boot context (`sched_init` adopts the boot stack). It
executes `sti; hlt` and yields when the tick marks a runnable thread. When
no thread is runnable, `sched_pick_next` returns the idle thread. Per CPU
run queues in M17 replace the single set of queues behind `sched_pick_next`.

## Blocking primitives

`waitq_wait(wq, held)` enqueues the caller under `wq->lock`, marks it
blocked under `sched_lock`, then releases the condition lock `held` and
switches. Because the waker needs `wq->lock` to find the thread and
`sched_lock` to change its state, no wakeup between the check of the
condition and the switch can be lost. Mutex, semaphore and condition
variable are built on this: each has a spinlock for its own state and a
wait queue. `condvar_wait` holds `condvar.lock` from before the mutex is
released until the thread is queued, so a signal in between is not missed.
Blocking primitives are never used from interrupt context.

`sleep_ms` blocks once the scheduler has started: the thread is inserted in
the sorted sleep list and the tick handler wakes it.

## Boot flow

`kmain` initializes the hardware, calls `proc_init` and `sched_init`,
enables interrupts, creates the `kinit` thread and enters the idle loop.
`kinit` runs the selected self test and, from M8, starts the first user
process. Panics print the current thread and process.

## Test

`tests/cases/sched` runs four workers at levels 0, 2, 4 and 6 incrementing a
mutex protected counter with a yield inside the critical section, a CPU
bound thread that is demoted through slice exhaustion, a sleeping thread, a
producer/consumer pair using a mutex, two condition variables and a
semaphore, and finally checks that the periodic boost returned `kinit` to
level 0.
