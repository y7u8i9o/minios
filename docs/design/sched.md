# Kernel threads and the per-CPU scheduler

## Thread ownership

`struct thread` holds the saved context, kernel stack, process, MLFQ level,
slice, home CPU and intrusive links used by ready, sleep, wait and remote-wake
queues.  A running thread and a thread linked on a ready or sleeper list are
owned by that CPU's run queue.  State changes are release-published because a
remote wake producer inspects the state before claiming `wake_node`.

Wait-queue membership is protected by the wait queue.  A remote waker removes
the thread from that list and publishes its `wake_node`; it never acquires the
destination run-queue lock.  A scheduler sleeper remains on its home CPU's
sleep list until that CPU consumes the wake notification.

## Run queues and remote wakeup

Every CPU has eight MLFQ lists, a sorted sleeper list, a `run_queue` spinlock
and an MPSC inbound list.  `sched_add` chooses the least-loaded started CPU.
`sched_wake` keeps an existing thread on its home CPU, changes
`wake_queued` from false to true and pushes the node with release ordering.
It sets `need_resched`; a remote target also receives `IRQ_RESCHED`, which
wakes an idle `hlt` and requests a scheduling point on return to user mode.

The target exchanges and reverses the inbound list under its local queue
lock.  It removes a scheduler sleeper from the local sleep list if necessary,
promotes a thread that blocked before exhausting its slice, and inserts the
thread once.  The consumer captures `node->next` and clears `wake_queued`
before it reads the thread's state, so a subsequent wake may safely reuse
the intrusive node and, more importantly, no wake is lost: the clear and
the compare-exchange of `sched_wake` are sequentially consistent, so a wake
that found the claim taken (and pushed nothing) is observed by the state
read that follows the clear.

A queued wake can be stale.  Signal delivery calls `waitq_interrupt` and
then `sched_wake` without holding the wait queue lock across both; when
the thread was woken by somebody else in between, ran, and blocked on a
wait queue again, the second call queues a thread that is legitimately
waiting.  The drain recognizes this as `THREAD_BLOCKED` with `waiting_on`
set and discards the entry; the eventual `waitq_wake_*` or interrupt
clears `waiting_on` under the queue lock before it queues the thread
again.  The case was found with the new init (2026-09-15), the first
process to sleep in `wait4` with a `SIGCHLD` handler installed: every
child exit wakes the parent's `child_waitq` and then sends the signal.

An empty CPU attempts to steal the highest-priority ready thread.  It holds
its local lock and uses `spin_try_lock` on each victim; it never waits for a
second queue lock.  This avoids a lock-order cycle between simultaneous
stealers.  The tick of an idle CPU requests a scheduling point when its own
queue or the queue of any other CPU has a ready thread (`others_have_ready`,
counts read without locks), so an idle CPU that missed the placement of new
threads steals one within a tick.  Before A8 only its own queue counted, and
under HVF a virtual CPU that the host descheduled while eight threads were
placed stayed idle for the rest of the `smp` case.

## Context switch

`context_switch(&old_sp, new_sp)` saves the six callee-saved registers and
the stack pointer, loads the next stack and returns into that context.  New
threads have a synthetic frame returning to `thread_start`.

`sched_switch_locked` is entered with the calling CPU's run-queue lock held
and current already changed from `THREAD_RUNNING`.  It drains inbound work,
picks or steals the next thread, updates `cpu.current`, TSS `rsp0`, the kernel
stack pointer, address space and FPU state, then switches while retaining the
local lock.  The resumed thread releases the lock belonging to the CPU on
which it actually resumed, so migration by stealing is safe.

A thread cannot free its own stack.  The old CPU records an exiting thread in
`zombie_pending`.  `sched_finish_switch`, running on the new stack, drops the
local queue lock, sets `finished`, wakes joiners and reacquires the lock before
returning to the switch path.  `thread_join` releases the stack only after
observing `finished`.

## MLFQ and timer work

Level 0 has a 10 ms slice and each lower level doubles it.  Only the owner CPU
touches the running thread's `slice_left`, so the timer decrements it without
a lock and release-stores `need_resched` at zero.  User mode is preempted on
interrupt return; kernel code remains cooperative and reaches scheduling
points when it blocks, yields or explicitly calls `sched_preempt`.

The tick takes the local queue lock to consume inbound wakes, expire the local
sorted sleeper list and perform that CPU's one-second priority boost.  A
thread that exhausts its slice is demoted at yield; a blocking thread is
promoted when the target CPU consumes its wake.

## Blocking and lost-wakeup rule

`waitq_wait(wq, held)` acquires `wq.lock`, links current, then acquires the
local run-queue lock and publishes `THREAD_BLOCKED`.  It releases `wq.lock`
and the caller's condition lock before switching.  A waker either sees no
waiter before registration or removes the registered waiter and publishes a
runnable MPSC node.  It does not need the local run-queue lock, so the old
global `condition -> waitq -> sched_lock` chain no longer exists.

RCU read sections may not enter this path.  The runtime check in
`waitq_wait` turns an attempted sleep inside RCU into a kernel panic rather
than allowing an unbounded grace period.

## Boot and tests

`sched_init` initializes every queue and the reschedule vector, adopts the
boot stack as CPU 0's idle thread and installs the timer hook.  Each AP adopts
its startup stack in `sched_init_cpu`.  Idle CPUs publish an RCU quiescent
state before `sti; hlt`.

`sched` covers priorities, sleep, condition variables, semaphores, joining
and the periodic boost.  `smp` verifies execution on all configured CPUs,
concurrent workers, migration and TLB shootdown.  `lockfree` additionally
stresses MPSC publication and per-CPU accounting.
