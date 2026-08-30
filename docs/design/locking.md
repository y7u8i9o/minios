# Locks and lock ordering

Every shared structure names its lock in a comment above its definition and
takes that lock from the first commit in which it exists. Disabling
interrupts is never sufficient on its own. New locks are added to this file
before the code that uses them.

## Locks

| Lock | Type | Protects | Introduced |
|---|---|---|---|
| `console_lock` | spinlock, irqsave | serial and framebuffer output state | M1 |
| `pmm_lock` | spinlock | buddy free lists, free counts, `pmm_stats`, `struct page` fields | M3 |
| `kvm_lock` (`kernel_vmspace.lock`) | spinlock | kernel page tables, kernel stack slot bitmap, MMIO bump pointer | M4 |
| `vmspace.lock` | spinlock | page tables of one user address space | M4 |
| `kmem_cache.lock` | spinlock | slab lists and free lists of one cache | M5 |
| `kmem_caches_lock` | spinlock | list of all caches | M5 |
| `kbd_lock` | spinlock | keyboard modifier state and line buffers, taken in the IRQ handler | M6 |
| `sched_lock` | spinlock | run queues, sleep list, thread scheduling fields, held across context switches, taken in the timer IRQ | M7 |
| `waitq.lock` | spinlock | waiters list of one wait queue | M7 |
| `mutex.lock`, `semaphore.lock` | spinlock | the state of one blocking primitive | M7 |
| `condvar.lock` | spinlock | orders waiter registration against signals | M7 |
| `proc.lock` | spinlock | thread list, state and exit status of one process | M7 |
| `proc_list_lock` | spinlock | list of all processes and pid counter | M7 |
| `proc_tree_lock` | spinlock | parent/children links and process state of every process | M9 |
| `tid_lock` | spinlock | thread id counter | M7 |
| `inode.lock` | mutex | contents, size and directory entries of one inode | M11 |
| `file.lock` | mutex | position of one open file description, held across the driver read or write | M11 |
| `superblock.lock` | spinlock | inode cache list and inode reference counts of one filesystem | M11 |
| `files_lock` | spinlock | reference counts of open file descriptions | M11 |
| `fdtable.lock` | spinlock | descriptor slots of one process | M11 |
| `mount_lock` | spinlock | mount table | M11 |
| `fs_types_lock` | spinlock | registered filesystem types | M11 |
| `devfs_lock` | spinlock | device node list | M11 |
| `pipe.lock` | spinlock | pipe ring buffer and end counts, condition lock of its wait queues | M11 |
| `virtqueue.lock` | spinlock | descriptor free list, ring indexes and completion cookies of one virtqueue, taken in the MSI-X handler, condition lock for request completion | M12 |
| `buf.lock` | mutex | data of one block cache buffer, held from `bread` to `brelse` | M12 |
| `bcache_lock` | spinlock | buffer LRU list, buffer identity, reference counts and flags | M12 |
| `blockdev_lock` | spinlock | list of block devices | M12 |
| `mfs_sb.lock` | mutex | inode and block bitmaps and the superblock counters of one mfs mount | M13 |
| `swap_io_lock` | mutex | serializes swap reads and writes | M14 |
| `vmspaces_lock` | spinlock | list of user address spaces walked by kswapd | M14 |
| `swap_lock` | spinlock | swap slot bitmap and counters, condition lock of `swap_waitq` | M14 |
| `proc.lock` (extended) | spinlock | also `sig_pending` and `sig_actions`; `pgid` joins `proc_tree_lock` | M15 |
| `mouse_lock` | spinlock | mouse packet assembly and event ring, taken in the IRQ handler, condition lock of `mouse_waitq` | M17 |
| `fbdev_lock` | spinlock | owner of the display | M17 |
| `shm_lock` | spinlock | table of named shared memory objects and their reference counts | M17 |
| `mqueue.lock` | spinlock | ring of one message queue, condition lock of its wait queues | M17 |
| `mq_table_lock` | spinlock | table of named message queues and their reference counts | M17 |
| `poll_lock` | spinlock | condition lock of `poll_waitq`, woken by every producer | M17 |
| `tty.lock` | spinlock | line discipline state and ready bytes of one terminal, taken in the keyboard IRQ for the console, condition lock of `tty.rd_waitq` and (console) `tty_intr_waitq`; replaces `kbd_lock` for that state | M17 |
| `kbd_lock` (reduced) | spinlock | keyboard modifier state only | M17 |
| `pty.lock` | spinlock | output ring of one pseudo terminal pair, condition lock of `pty.out_waitq` | M17 |
| `pty_table_lock` | spinlock | allocation of pseudo terminal pairs | M17 |
| `tlb_lock` | spinlock | the TLB shootdown request in flight and its statistics, held by the sender while it waits for acknowledgements | M18 |

## Ordering

Locks are listed from outermost to innermost. A CPU holding a lock may only
acquire locks that appear later in this list.

1. `file.lock` (mutex)
2. `inode.lock` (mutex, two directories in inode number order for rename)
3. `mfs_sb.lock` (mutex)
4. `buf.lock` (mutex)
5. `swap_io_lock` (mutex)
6. `condvar.lock`
7. `proc_tree_lock`
8. `mutex.lock`, `semaphore.lock`, `proc.lock`, `thread.exit_lock`, `kbd_lock`, `pipe.lock`, `virtqueue.lock`, `swap_lock`, `mouse_lock`, `mqueue.lock`, `poll_lock`, `tty.lock`, `pty.lock` (condition locks passed to `waitq_wait`)
9. `waitq.lock`
10. `sched_lock`
11. `vmspaces_lock`, then `vmspace.lock` (user spaces)
12. `kvm_lock`
12a. `tlb_lock` (taken inside any `vmspace.lock` or `kvm_lock` by `tlb_flush_range`, and by `vmspace_destroy` with no space lock held)
13. `kmem_caches_lock`
14. `kmem_cache.lock`
15. `pmm_lock`
16. `proc_list_lock`, `tid_lock`, `fdtable.lock`, `files_lock`, `superblock.lock`, `mount_lock`, `fs_types_lock`, `devfs_lock`, `bcache_lock`, `blockdev_lock`, `fbdev_lock`, `shm_lock`, `mq_table_lock`, `pty_table_lock`
17. `console_lock`

`proc_tree_lock` sits above `proc.lock` because `wait4` reads the exiting
flag of the caller while scanning its children. `kbd_lock` is a condition
lock for `ps2kbd_read` and is also taken in the keyboard interrupt, which
is safe because every spinlock disables interrupts. The page fault handler
takes `vmspace.lock` and then `pmm_lock` from exception context.

Blocking primitives never run in interrupt context. `sched_lock` is held
across `context_switch` and released by the resumed thread, so nothing may
be allocated or freed while it is held; `sched_switch_locked` only touches
scheduler state and the TSS.

Page table changes allocate table pages, so `kvm_lock` and `vmspace.lock`
sit above `pmm_lock`. Slab caches take pages from the buddy allocator, so
`kmem_cache.lock` sits above `pmm_lock` as well. A user space lock is above
`kvm_lock` because kernel stack allocation for a new thread can happen while
setting up a process.

The filesystem mutexes are outermost because a driver read or write runs
with `file.lock` held and may block on the keyboard or a pipe, taking
their condition locks inside. `pipe.lock` and `kbd_lock` read `proc.lock`
to check the exit flag before blocking, so they sit at the same level as
the other condition locks and never nest with each other. The leaf
spinlocks in level 16 protect reference counts and small tables and take
no other lock; `fdtable.lock` is released before `file_put` runs so a
release callback may sleep.

`mfs_sb.lock` is taken while an inode mutex is held (allocating a block
for a file) and takes buffer locks for the bitmap blocks, so it sits
between them. `buf.lock` sits below the inode mutex because a filesystem operation
reads and writes blocks while holding its inode, and above the condition
locks because a block transfer sleeps on `virtqueue.lock`. `bcache_lock`
is a leaf: it is dropped before any buffer mutex or device transfer.

`vmspaces_lock` is held by kswapd while it takes one `vmspace.lock` at a
time; nothing takes two space locks at once. `swap_lock` is taken under
`vmspace.lock` (slot allocation and release while an entry is rewritten),
which places it below the space locks; as the condition lock of
`swap_waitq` it is also taken on its own. `swap_io_lock` is taken by
kswapd and by the fault handler with no spinlock held and sleeps on the
block device below it. Since M18 kswapd holds it across the whole
eviction batch, from before an entry is rewritten to its swap slot until
the frame data is written, so a swap in on another CPU cannot read a slot
whose data is still in flight.

`signal_send` takes `proc.lock` and then `waitq.lock` through
`waitq_interrupt`, and `proc_exit_notify` sends `SIGCHLD` with no lock
held. `proc_collect_pgrp` takes `proc_tree_lock` and then
`proc_list_lock` and returns pids, so the signals are posted afterwards
without either lock. The control C path in the keyboard interrupt only
wakes `ttyd` under `kbd_lock`; `ttyd` posts the signal from thread
context.

`tty_input_char` on a pseudo terminal runs in the writer's process
context and posts `SIGINT` after dropping `tty.lock`; on the console it
only sets a flag under `tty.lock` and wakes `ttyd`. A pseudo terminal's
`tty.output` callback takes `pty.lock` while `tty.lock` is held, so
`pty.lock` is never taken first.

`tlb_lock` is taken with a space lock held and the sender spins on it
with interrupts disabled while waiting for other CPUs. A CPU that spins
on any spinlock services pending shootdown requests from its spin loop
(`tlb_shootdown_poll`), so a target that waits for a lock the sender
holds still acknowledges the request. Nothing is acquired under
`tlb_lock` except `console_lock` through the interrupt path. The sender
never targets itself.

`sched_lock` stays a single lock covering the run queues of every CPU. It
is taken by the timer interrupt of every CPU and held across every
context switch; the per CPU fields of `struct cpu` that the scheduler
writes (`current`, `idle`, `need_resched`, `zombie_pending`) are protected
by it as well.

`console_lock` is innermost because any subsystem may print while holding its
own lock. Code holding `console_lock` must not call into any other subsystem.
The panic path bypasses `console_lock` once `panic_in_progress` is set.

## M23 additions

- `sock_table_lock` (listener names) -> `sock->lock` (a listener's
  backlog). `conn->lock` (the two rings and descriptor records of a
  connection) is taken alone; `poll_notify` is called after it is
  released.
- `timerfd_lock` protects the armed timer list and every timer's
  fields; it is taken from the timer interrupt and is the condition
  lock of the timers' wait queues.
- `timed_lock` (timed wait queue waiters) is taken after the caller's
  condition lock in `waitq_wait_timeout`, and before `wq->lock` inside
  `waitq_interrupt` from the timer interrupt.
- `eventfd->lock` is leaf.
