# Lock-free and per-CPU paths (M43-M46)

M43-M46 replace the global locks on the common system-call, scheduling,
byte-stream and allocation paths.  The kernel remains non-preemptible:
interrupts can interrupt kernel code, but another thread does not run on the
same CPU until the current thread blocks, yields or returns through a
preemption point.  This property is part of the proofs below.

## Ordering rules and primitives

All atomic operations name their ordering in `kernel/sync/atomic.h`.
Statistics use relaxed operations.  A pointer or index that makes initialized
memory visible is stored with release ordering and loaded with acquire
ordering.  A reference count reaching zero is terminal:
`refcount_inc_not_zero` is the only operation allowed after a lock-free
lookup, and destruction uses a release decrement followed by an acquire
fence.

`percpu.h` stores one counter in each cache line.  The owner CPU updates its
slot and readers sum the slots.  `vmspace.resident` uses this form, so process
accounting no longer walks page tables.

`mpsc.h` is an intrusive multi-producer/single-consumer stack.  Producers
publish a node with a release CAS.  The consumer acquires the complete list
with one exchange and reverses it to recover FIFO order.  Only the consumer
removes nodes, so the head operation has no ABA case.  Scheduler wake inboxes
and RCU callback queues use this primitive.

`ring.h` is a power-of-two SPSC byte ring.  The producer owns `head`, writes
the bytes, then publishes `head` with release ordering.  The consumer owns
`tail`, reads bytes after acquiring `head`, then releases the new `tail`.
Blocking and readiness notification remain outside the ring.

`rcu.h` implements epoch RCU for the non-preemptible kernel.  Read sections
may not sleep; `waitq_wait` detects this invariant.  Context switches and the
idle loop publish quiescent epochs.  `rcu_call` attaches the next epoch to an
intrusive callback and puts it on the calling CPU's MPSC queue. A dedicated
`rcu` kernel thread is the sole consumer of all CPU queues and reclaims a
callback after every started CPU has published that epoch. It sleeps one
millisecond between scans and holds no RCU lock across a callback.
Callbacks may sleep: releasing a VMA's last file reference can enter an MFS
journal transaction. Running that path from the timer interrupt could block
an interrupted idle thread and corrupt its wait-queue membership.

## File descriptors, signals and VM regions

`proc.exiting` and `proc.sig_pending` are published atomically.  The common
signal and exit checks perform acquire loads and avoid `proc.lock` when no
work is pending.

`struct file.refcount` is a `refcount_t`; the former global `files_lock` no
longer exists.  A descriptor slot is installed with a release store.
`fdtable_get` enters an RCU read section, acquires the slot, and calls
`refcount_inc_not_zero`.  Close publishes NULL before dropping the table's
reference.  The file's release operation and inode reference are discharged
at the last reference; the file allocation itself remains available until an
RCU callback, covering a reader that loaded the old slot immediately before
close.

The VMA list has an RCU-published forward link.  Writers still hold
`vmspace.lock` for list surgery and page-table changes.  Removal updates the
predecessor with a release store and leaves the removed node's old forward
link intact until its callback.  `vma_range_ok` therefore walks the list
without `vmspace.lock`; an old reader either follows the old valid chain or
observes the new one.  File and mapping references attached to a removed VMA
are also released from the callback.

## Object-local poll

Every pollable object exposes a `poll_source`.  `poll_files` registers one
stack entry with each distinct source before its first readiness check.  A
producer walks only that source's waiter list, sets the waiter's notification
word and wakes its private wait queue.  Clearing the word, checking readiness
and registering for sleep are ordered by the waiter lock, so a transition in
the check-to-sleep window is retained.  Entries are removed before the
function returns.

Pipes, ttys, ptys, sockets, message queues, eventfd, timerfd, mouse, klog,
profiler, PCM and virtqueue-backed devices notify their own sources.  The
former global poll lock, generation and wake queue have been removed; a write
to one object cannot cause a polling herd on unrelated descriptors.

## Per-CPU scheduler

Each CPU owns one MLFQ, one sorted sleeper list and one MPSC runnable inbox.
The run-queue lock protects only that CPU's lists.  Remote wakeup atomically
claims the thread's intrusive wake node, publishes it to the home CPU and
sends `IRQ_RESCHED`.  The target drains the inbox under its own lock.  A
thread cannot be inserted twice because only the producer that changes
`wake_queued` from false to true may publish the node.

The timer decrements the locally running thread's slice without a lock.  It
takes the local queue lock only to drain wakeups, expire local sleepers and
perform the local one-second boost.  An empty CPU tries victim locks while
holding its own lock; a failed try is skipped, so two stealing CPUs never
wait on each other.  The old global `sched_lock` has been removed.  Full
context-switch and wait-queue details are in `sched.md`.

## Byte streams, input, logging and profiling

The byte storage of pipes, tty ready input, pty output and each socket
direction uses `spsc_ring`.  Socket descriptor-passing records and endpoint
state remain under the connection lock because they update several fields as
one transaction.  Mouse events are fixed-size records in a byte ring.  The
keyboard feeds the console tty ring.

Console and klog producers have one SPSC staging ring per CPU.  Interrupts
are disabled only around the owner CPU's enqueue, which serializes thread and
interrupt producers on that CPU without a cross-CPU lock.  `consoleout`
merges the rings and is the sole ordinary UART writer.  Its drain mutex keeps
an explicit flush ordered with the daemon, while the slow polled UART runs
without `console_lock`; that spinlock now covers only framebuffer state and
the short framebuffer write.  The panic path bypasses the queue and writes
directly.  `/dev/klog` retains a bounded chronological history; its global
history lock is taken by the single drain side, not by logging CPUs.

The profiler has one fixed-record SPSC ring per CPU.  A local timer interrupt
is its sole producer, and `/dev/profile` is the consumer that merges the
rings.  A short per-CPU active count lets close stop sampling, wait for
in-flight interrupts and safely release the ring pages.

## Allocators and address spaces

Each slab cache has a small magazine per CPU.  Normal allocation and free
touch the local magazine lock only; the cache lock is needed for a batch
refill or drain.  `slab_reclaim` drains all magazines for memory-pressure and
exact-accounting paths, allowing completely free slabs to return to the buddy
allocator.  Each CPU also caches order-zero physical pages behind a local
cache lock, refilling and draining in batches.  The local lock is released
before the buddy lock is taken.  The exported free-page count includes these
lists, and `pmm_reclaim_cpu_caches` drains them for high-order pressure and
exact coalescing checks.

`vma_populate` allocates and zeroes both the user frame and enough possible
page-table frames before taking `vmspace.lock`; the locked walk consumes only
the table frames it needs and the caller returns unused frames after unlocking.
Anonymous fault-in likewise zeroes its user frame before reacquiring the
space lock, then revalidates the VMA and page-table slot before publication.
Every present user mapping path increments `vmspace.resident`, and
unmap, swap-out and lazy-free paths decrement it. `munmap` clears all entries
under the space lock, detaches empty leaf page tables, and issues one range
TLB shootdown before returning those table frames to the buddy allocator.
Keeping empty tables allocated can fragment otherwise free 2 MiB blocks
after a large small-page workload. Swap and `PROT_NONE` entries keep their
tables alive, and absent upper levels are skipped for sparse ranges.
Whole-space teardown omits that redundant range round because the immediately
following `vmspace_destroy` drops the address space from every CPU before it
frees the page tables.

## Ordering audit for aarch64 (A8)

The aarch64 port runs these paths on a weakly ordered processor, where an
acquire load does not order an earlier plain store and a control
dependency does not order a later load. An audit of the spinlock and of
every path above found five orderings that x86 permitted and the C11
model does not. A8 corrected them.

- The wakers of a wait queue cleared `waiting_on` with a plain store
  before the claim exchange of `queue_inbound`. A drain could then read
  the old queue and discard the wake as stale, and the thread never ran
  again. `wake` and `waitq_interrupt` now store `NULL` sequentially
  consistent, and `waitq_wait` stores `waiting_on` and the blocked state
  as atomics.
- A shootdown target read the request before the bit of `pending` that
  publishes it. `tlb_shootdown_poll` now loads `pending` with acquire.
- `virtq_drain_locked` read a used ring entry before the used index. It
  now reads the index, executes `rmb` and then reads the entries up to it.
- The profiler's `quiesce` and `ring_enter` formed a store and load pair
  on two variables with release and acquire only. Both sides are now
  sequentially consistent.
- `tlb_flush_range` read `cpu_mask` after it cleared the entries, against
  the `fetch_or` and the table walk of `vmspace_activate`. A sequentially
  consistent fence now separates the stores of the entries from the load
  of the mask.

The spinlock is correct with the exchange and the release store that it
uses. The audit also found five races that occur under any memory order,
and A8 corrected them.

- `pts_write` wrote the pty output ring without `p->lock`, while the echo
  of `pty_output` wrote the same ring under the lock. The write is now
  under the lock.
- `vma_split_locked` shrank the region before it linked the tail, so a
  lockless `vma_range_ok` could find a gap. The tail is now linked first.
- A signal sent after a caller checked for signals and before
  `waitq_wait` registered the thread found no queue to interrupt.
  `waitq_signal` sets the one-shot flag `sig_wake` before it interrupts
  the wait, and `waitq_wait` exchanges the flag after it registers the
  thread and returns at once if the flag was set.
- `stop_current` read `stopped` and then stored the stopped state, so a
  `SIGCONT` between the two left the thread stopped. The thread now
  stores the state and reads `stopped` again, both sequentially
  consistent, against the store of `continue_process` and the state load
  of `sched_wake`.
- `filemap_put` reached zero without `filemap_lock`, so a lookup could
  revive the mapping and both callers freed it. The last reference is now
  dropped under the lock.

## Validation

`lockfree` exercises atomic counters, reference counts, per-CPU counters,
MPSC publication and a sleeping RCU callback across four CPUs. The callback
asserts that interrupts are enabled before sleeping. `poll_wake` measures
an object-local pipe transition.  `ring` verifies full/empty and wraparound
transitions with producer and consumer scheduled independently.  The existing
`sched`, `smp`, `pipes`, `pty`, `sockets`, `profile`, `slab`, `pmm`, `vmm`,
`swap` and `hugepages` cases cover the migrated subsystems.
