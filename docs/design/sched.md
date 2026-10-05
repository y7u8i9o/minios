# Kernel threads and the per-CPU scheduler

## Thread ownership

`struct thread` contains the saved context, kernel stack, process, MLFQ level,
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
`sched_wake` retains an existing thread on its home CPU, changes
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
then `sched_wake` without acquiring the wait queue lock across both; when
the thread was woken by somebody else in between, ran, and blocked on a
wait queue again, the second call queues a thread that is legitimately
waiting.  The drain recognizes this as `THREAD_BLOCKED` with `waiting_on`
set and discards the entry; the eventual `waitq_wake_*` or interrupt
clears `waiting_on` under the queue lock before it queues the thread
again.  The case was found with the new init (2026-09-15), the first
process to sleep in `wait4` with a `SIGCHLD` handler installed: every
child exit wakes the parent's `child_waitq` and then sends the signal.

A wait queue may lie on the stack of its waiter, as in `futex_wait`.
`waitq_interrupt` reads `waiting_on` and then acquires the lock of that
queue. When the waiter was woken in between and returned, the queue was
gone, and the lock of unrelated stack data was acquired and released. The
`prof_gui` case ended with "spinlock futex: release by non owner" when it
killed the profiled processes (2026-10-05). `waitq_interrupt` now pins the
thread (`waitq_pins`, atomic) before it reads `waiting_on` and unpins it
after it releases the queue lock, and `waitq_wait` returns only when the
pins are zero. An interrupt that pins the thread after the wake cleared
`waiting_on` reads `NULL`, because both sides are sequentially consistent.

`futex_wait` compares the futex word under the bucket spinlock. A word on
a page that is not present faulted under the lock, and the fault slept on
disk I/O. Since program data is mapped from the file this happens for a
futex in the data of a program. The word is now touched before the lock
and read under the lock through the page tables, and the lock is released
and the word touched again when the page left in between.

An empty CPU attempts to steal the highest-priority ready thread.  It acquires
its local lock and uses `spin_try_lock` on each victim; it never waits for a
second queue lock.  This avoids a lock-order cycle between simultaneous
stealers.  The tick of an idle CPU requests a scheduling point when its own
queue or the queue of any other CPU has a ready thread (`others_have_ready`,
counts read without locks), so an idle CPU that missed the placement of new
threads steals one within a tick.  Before A8 only its own queue counted, and
under HVF a virtual CPU that the host descheduled while eight threads were
placed remained idle for the rest of the `smp` case.

## Context switch

`context_switch(&old_sp, new_sp)` saves the six callee-saved registers and
the stack pointer, loads the next stack and returns into that context.  New
threads have a synthetic frame returning to `thread_start`.

`sched_switch_locked` is entered with the calling CPU's run-queue lock acquired
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

A sleeper that the tick wakes preempts the running thread: the tick sets
`need_resched` when it has woken a sleeper and the CPU runs another thread
than its idle thread.  A wake through `sched_wake` already did so through
`kick_cpu`.  Until 2026-10-05 the tick requested a reschedule only on an
idle CPU.  A woken sleeper then waited for the rest of the slice of the
running thread, up to 1280 ms on the lowest level.  With one busy process
per CPU, a 10 ms `usleep` overshot by 163 ms on average and by up to 388 ms.  Every
timed sleep of the kernel (`sleep_ms`) and of user programs (`sleep`,
`usleep`, `nanosleep`) was affected whenever the CPUs were busy.  The case
`sleep_latency` measures fifty 10 ms sleeps beside one busy process per
CPU and requires an overshoot below 15 ms.  Since the correction the
overshoot is about 1.2 ms, the resolution of the tick
(`docs/postmortems/2026-10-05-sleep-wakeup.md`).

`sched_sleep_until(tick, interrupted)` records `sleep_cpu`, the CPU whose
sleeper list contains the thread, and publishes `THREAD_SLEEPING` with
sequential consistency before it calls `interrupted`. `sched_wake` sends
the wake of a sleeper to `sleep_cpu`, and a drain that finds a sleeper of
another CPU forwards the wake there, because only the CPU of a sleeper list
changes that list. A wake can end a sleep before its tick. `sleep_ms`
therefore sleeps again until the deadline, and `sleep_ms_interruptible`
ends early only when `interrupted` returns true. The system call `sleep_ms`
passes `signal_should_interrupt` and returns `-EINTR` for a signal, a stop
or the exit of the process. The kernel test `sched_wake_race` forces stale
wakes and requires that every sleep lasts its full time.

## Blocking and lost-wakeup rule

`waitq_wait(wq, lock)` acquires `wq.lock`, links current, then acquires the
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
and the periodic boost.  `sleep_latency` covers the wake-up latency of a
timed sleep beside busy processes.  `sched_quiet` reports whether no thread
except a given one runs or is ready on any CPU, and whether no thread of a
user process sleeps until a tick before a limit.  The boot tests wait
through it for the processing of their input (`ktest_wait_idle`,
`build.md`).  `smp` verifies execution on all configured CPUs,
concurrent workers, migration and TLB shootdown.  `lockfree` additionally
stresses MPSC publication and per-CPU accounting.
