# Lock-free data structures: design and plan

This document is the plan for milestones M43 to M46 and will be updated
as each is implemented. The measurements it rests on are in
`lockstat.md`.

## Rules

- Spinlocks stay where a critical section changes several words at once
  and is short: run queue manipulation, list surgery, table growth. The
  goal is to take no spinlock on the paths every system call runs and to
  make the locks that remain per CPU or per object.
- Atomics use the `__atomic` builtins with explicit ordering: `RELAXED`
  for counters nobody reads for control decisions, `ACQUIRE`/`RELEASE`
  pairs for publishing a structure and consuming it, `SEQ_CST` only where
  a proof needs it. Every lock-free structure documents its ordering in a
  comment.
- No ABA is possible where only one side can pop: the MPSC stack below
  has many pushers and one popper, so a CAS on the head from the popper
  cannot see a recycled node. Structures with several poppers are not
  used.
- Freed memory that lock-free readers may still be looking at is
  released through `rcu_free` (below), never through `kfree` directly.
- Every structure comes with a self test under `tests/cases/` that runs
  producers and consumers on all four CPUs and checks counts, and the
  `lockstat` rows named in each milestone must reach the stated counts
  in `prof_gui`.

## Primitives (`kernel/sync/`)

`atomic.h`: `atomic_inc`, `atomic_dec_and_test`, `atomic_cmpxchg`
wrappers naming the ordering, and `refcount_t` with `refcount_inc_not_zero`
for the "reference an object I found through a lock-free lookup" case.

`percpu.h`: per CPU counters (`struct percpu_counter`, one cache line per
CPU indexed by `cpu_current()->id`, summed when read). Used for the
allocator statistics, the fault and switch counters and the resident
page count of an address space.

`ring.h`: single producer, single consumer byte ring. The producer owns
`head`, the consumer owns `tail`; each publishes its index with a
`RELEASE` store after writing or reading data and reads the other's with
an `ACQUIRE` load. Capacity is a power of two. Blocking stays outside the
ring: a wait queue per side, woken after the index store.

`mpsc.h`: intrusive multi producer, single consumer stack. Producers push
with a CAS loop on `head`; the consumer takes the whole list with one
`xchg` of `head` to NULL and processes it in reverse. This is the
wakeup path between CPUs and the deferred free list of RCU.

`rcu.h`: read-copy-update for a kernel that is not preemptible. A read
side critical section is any kernel code that does not block, so
`rcu_read_lock` is a compiler barrier and a debug counter. A writer
publishes a new version with a `RELEASE` store of a pointer and calls
`rcu_free(old)`, which pushes the old memory onto a per CPU MPSC list
with the current grace period number. Every CPU increments its own
counter in `sched_switch_locked` and in the idle loop; a grace period
ends when every started CPU has passed such a point since the item was
queued, checked by the CPU that runs `rcu_reclaim` from its tick. Sleeping
inside a read side section is a bug that the debug counter catches in
`waitq_wait`.

`seqlock.h`: a sequence counter for small structures that are read often
and written rarely (the time offset of `clock_settime`, compositor
settings if they move into the kernel). Readers retry while the sequence
is odd or changed.

## Hot paths and their replacements

### System call entry and exit (M43)

`proc.exiting` and `proc.sig_pending` become atomic words.
`proc_exit_check`, `signal_should_interrupt` and `signal_deliver` read
them with `ACQUIRE` loads and take `proc.lock` only when a bit is set,
which is the rare case. The six `proc.lock` acquisitions per system call
become zero on the common path.

`user_range_ok` stops walking the region list under `vmspace.lock`. The
region list becomes RCU protected: `vma_split_locked`, `vma_munmap` and
friends still take the lock to change it, but freed regions go through
`rcu_free` and the lookup for a user pointer check runs without the lock.
The later step, planned in the same milestone if time allows, is
`copy_from_user`/`copy_to_user` with fault fixup: the kernel touches the
user address directly, the page fault handler recognizes a fault inside
an accessor and returns `-EFAULT` through a fixup table, and the region
walk disappears from the system call path entirely.

`struct file.refcount` becomes a `refcount_t` and `files_lock` is
removed. `fdtable_get` reads `fds[fd]` with an `ACQUIRE` load and takes a
reference with `refcount_inc_not_zero`; `fdtable_close` publishes NULL,
drops the table's reference and frees the file through `rcu_free` when
the count reaches zero, so a reader that loaded the pointer just before
the close either gets a reference or sees the count at zero and retries
as "no such descriptor". `fdtable.lock` stays for install and close.

`poll` loses the global wait queue. Every pollable object gets a
`struct waitq` of its own (sockets already have per direction queues,
pipes, ttys, timers and the input rings gain one), and `poll_files`
registers a waiter entry on the queue of every descriptor before it
checks readiness, then sleeps; a producer wakes only the queues of its
own object. `poll_notify`, `poll_lock` and `poll_generation` are removed.
Readiness checks (`sock_poll`, `tty_poll`, ...) read their counts with
atomic loads instead of taking the object lock.

Target for `prof_gui`: the `proc`, `files_lock` and `poll_lock` rows are
gone or below one thousand acquisitions, `vmspace` drops to page faults
and mmap calls only, and a `poll` on one socket takes at most the
socket's lock.

### Scheduler (M44)

`sched_lock` splits into one `struct run_queue.lock` per CPU. A CPU takes
only its own lock to pick the next thread and to enqueue a thread that
became ready on itself. Waking a thread that belongs to another CPU
pushes it onto that CPU's `mpsc` wakeup list without taking any lock and
sends a reschedule IPI when the target is idle; the target drains the
list under its own lock at its next scheduling point. Work stealing takes
the victim's lock only, never two locks at once, in CPU id order for the
two moments it needs both (moving a thread between queues at boost time).
The tick decrements `slice_left` of the running thread without a lock,
since only the running CPU touches it. Sleepers move to a per CPU sorted
list under the run queue lock; the boost walks each CPU's queues under
that CPU's lock.

Target: the `sched_lock` row disappears, the `run_queue` rows sum to the
old acquisition count with contention under one percent, and the timer
tick takes no lock on an idle CPU.

### Byte streams and the console (M45)

The data path of pipes, pseudo terminals, the console tty, sockets (one
`ring` per direction, the descriptor passing records keep the lock) and
the kernel log becomes `ring`. The input rings filled from the mouse and
keyboard interrupts become `ring` as well, so the interrupt handler never
takes a lock that a reader may hold.

`console_write` appends to the klog ring lock-free and wakes a console
thread that drives the UART and the framebuffer console; the lock hold
of a write falls from hundreds of microseconds to the copy of the line.
Panic output keeps the direct path.

The profiler ring becomes one `ring` of samples per CPU so the timer
interrupt of one CPU never contends with another; the reader merges.

### Allocators and address spaces (M46)

`kmem_cache` gets a per CPU magazine (a small array of free objects owned
by one CPU) in front of the slab lists, so `kmalloc` and `kfree` of small
objects take no lock; the slab lock is taken to refill or drain a
magazine. `pmm_alloc_page` and `pmm_free_page` get a per CPU list of
single pages in front of the buddy lists. `pmm_stats` and the swap,
filemap and huge page counters become per CPU counters.

`vma_populate` allocates and zeroes pages before taking `vmspace.lock`
and maps them under it. `struct vmspace` gets a `resident` per CPU
counter maintained where entries are installed and cleared, so
`/dev/proc` and `getrusage` stop walking page tables. TLB shootdowns for
`munmap` of many pages are batched into one round per call.

## Order of work

M43 first: it removes the per system call locks that every process pays
for and the poll herd that wakes every client on every event. M44 next,
because the scheduler lock is the most contended one after the poll lock.
M45 and M46 follow; the console change in M45 is small and may be done
first within the milestone since it has the largest single hold time.
