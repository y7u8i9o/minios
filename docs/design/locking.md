# Locks and lock ordering

Every shared structure names its lock in a comment above its definition and
takes that lock from the first commit in which it exists. Disabling
interrupts is never sufficient on its own. New locks are added to this file
before the code that uses them.

## Locks

| Lock | Type | Protects | Introduced |
|---|---|---|---|
| `console_lock` | spinlock, irqsave | direct early serial output and framebuffer console state; async UART output runs outside it | M1/M45 |
| `pmm_lock` | spinlock | buddy free lists, free counts, `pmm_stats`, `struct page` fields | M3 |
| `kvm_lock` (`kernel_vmspace.lock`) | spinlock | kernel page tables, kernel stack slot bitmap, MMIO bump pointer | M4 |
| `vmspace.lock` | spinlock | page tables of one user address space | M4 |
| `kmem_cache.lock` | spinlock | slab lists and free lists of one cache | M5 |
| `kmem_caches_lock` | spinlock | list of all caches | M5 |
| `kbd_lock` | spinlock | keyboard modifier state and line buffers, taken in the IRQ handler | M6 |
| `run_queue.lock` | per-CPU spinlock | that CPU's ready queues, sleeper list and scheduling transitions; held across context switches | M44 |
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
| `fdtable.lock` | spinlock | descriptor slots of one process | M11 |
| `mount_lock` | spinlock | mount table | M11 |
| `fs_types_lock` | spinlock | registered filesystem types | M11 |
| `devfs_lock` | spinlock | device node list | M11 |
| `pipe.lock` | spinlock | pipe end counts and lost-wakeup checks; byte indexes are SPSC atomics | M11/M45 |
| `virtqueue.lock` | spinlock | descriptor free list, ring indexes and completion cookies of one virtqueue, taken in the MSI-X handler, condition lock for request completion | M12 |
| `buf.lock` | mutex | data of one block cache buffer, held from `bread` to `brelse` | M12 |
| `bcache_lock` | spinlock | buffer LRU list, buffer identity, reference counts and flags | M12 |
| `blockdev_lock` | spinlock | list of block devices | M12 |
| `mfs_sb.lock` | mutex | inode and block bitmaps and the superblock counters of one mfs mount | M13 |
| `mfs_journal.lock` | spinlock | operation and reservation counters, the committing flag and the pinned buffer list of one mfs journal; condition lock of its wait queue | M36 |
| `fat_sb.lock` | mutex | the allocation table, the free cluster count and the search hint of one FAT mount | M36 |
| `swap_io_lock` | mutex | serializes swap reads and writes | M14 |
| `vmspaces_lock` | spinlock | list of user address spaces walked by kswapd | M14 |
| `swap_lock` | spinlock | swap slot bitmap and counters, condition lock of `swap_waitq` | M14 |
| `proc.lock` (extended) | spinlock | also `sig_pending` and `sig_actions`; `pgid` joins `proc_tree_lock` | M15 |
| `mouse_lock` | spinlock | mouse producer serialization and condition checks; event indexes are SPSC atomics | M17/M45 |
| `fbdev_lock` | spinlock | owner of the display | M17 |
| `pcm_device.owner_lock` | spinlock | exclusive owner of one raw PCM device | audio |
| `virtio_snd.control_lock` | mutex | serializes one sound device's set-params, prepare, start, stop and release commands | audio |
| `shm_lock` | spinlock | table of named shared memory objects and their reference counts | M17 |
| `mqueue.lock` | spinlock | ring of one message queue, condition lock of its wait queues | M17 |
| `mq_table_lock` | spinlock | table of named message queues and their reference counts | M17 |
| `poll_source.lock` | per-object spinlock | poll waiter entries registered on that object only | M43 |
| `tty.lock` | spinlock | line discipline state and ready bytes of one terminal, taken in the keyboard IRQ for the console, condition lock of `tty.rd_waitq` and (console) `tty_intr_waitq`; replaces `kbd_lock` for that state | M17 |
| `kbd_lock` (reduced) | spinlock | keyboard modifier state only | M17 |
| `pty.lock` | spinlock | output ring of one pseudo terminal pair, condition lock of `pty.out_waitq` | M17 |
| `pty_table_lock` | spinlock | allocation of pseudo terminal pairs | M17 |
| `tlb_lock` | spinlock | the TLB shootdown request in flight and its statistics, held by the sender while it waits for acknowledgements | M18 |
| `prof_lock` | spinlock | profiler session reconfiguration; timer samples use per-CPU rings | M41/M45 |
| `slab_magazine.lock` | per-cache/per-CPU spinlock | one CPU magazine; cross-CPU use occurs only during reclaim | M46 |
| `pmm_cpu_cache` | per-CPU spinlock | one CPU's cached order-zero physical pages | M46 |
| `filemap_lock` | spinlock | `inode->mapping` pointers and the reference counts of mappings | M37 |
| `mapping.lock` | mutex | the page array of one file mapping, held while a page is read from the file or written back | M37 |
| `mapping.dirty_lock` | spinlock | the dirty bitmap of one file mapping, set while a `vmspace.lock` is held | M37 |
| `socket.lock` | spinlock | the pending asynchronous error of one socket | N01 |
| `families_lock`, `inet_protocols_lock` | spinlock | the socket family table and the Internet protocol table | N01 |
| `net_worker.lock` | spinlock | the worker's packet and request queues, timer list, kick flag and counters; condition lock of its sleep and of request completion | N02 |
| `pbuf_pool.lock` | spinlock | the packet buffer free list and counters | N02 |
| `netif_lock` | spinlock | the interface list and the flags of every interface | N02 |

## Ordering

Locks are listed from outermost to innermost. A CPU holding a lock may only
acquire locks that appear later in this list.

1. `file.lock` (mutex)
1a. `virtio_snd.control_lock` (mutex)
2. `inode.lock` (mutex, two directories in inode number order for rename)
3. `mfs_sb.lock`, `fat_sb.lock` (mutex)
4. `buf.lock` (mutex)
5. `swap_io_lock` (mutex)
6. `condvar.lock`
7. `proc_tree_lock`
8. `mutex.lock`, `semaphore.lock`, `proc.lock`, `thread.exit_lock`, `kbd_lock`, `pipe.lock`, `virtqueue.lock`, `swap_lock`, `mouse_lock`, `mqueue.lock`, `poll_source.lock`, `tty.lock`, `pty.lock`, `net_worker.lock` (condition and notification locks above private wait queues)
9. `waitq.lock`
10. the calling CPU's `run_queue.lock`
11. `vmspaces_lock`, then `vmspace.lock` (user spaces)
12. `kvm_lock`
12a. `tlb_lock` (taken inside any `vmspace.lock` or `kvm_lock` by `tlb_flush_range`, and by `vmspace_destroy` with no space lock held)
13. `kmem_caches_lock`
14. `slab_magazine.lock`, then `kmem_cache.lock` on refill, drain or reclaim
15. `pmm_lock`
16. `pmm_cpu_cache`, `proc_list_lock`, `tid_lock`, `fdtable.lock`, `superblock.lock`, `mount_lock`, `fs_types_lock`, `devfs_lock`, `bcache_lock`, `blockdev_lock`, `fbdev_lock`, `pcm_device.owner_lock`, `shm_lock`, `mq_table_lock`, `pty_table_lock`, `socket.lock`, `families_lock`, `inet_protocols_lock`, `pbuf_pool.lock`, `netif_lock`
17. `console_lock`

`proc_tree_lock` sits above `proc.lock` because `wait4` reads the exiting
flag of the caller while scanning its children. `kbd_lock` is a condition
lock for `ps2kbd_read` and is also taken in the keyboard interrupt, which
is safe because every spinlock disables interrupts. The page fault handler
takes `vmspace.lock` and then `pmm_lock` from exception context.

Blocking primitives never run in interrupt context.  A CPU's run-queue lock
is held across `context_switch` and released by the resumed thread on its new
CPU, so nothing is allocated or freed while it is held.  Remote wakers publish
through MPSC and therefore do not nest a destination queue lock under a
condition or wait-queue lock.

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
`waitq_interrupt`, and `proc_exit_notify` sends `SIGCHLD` with no lock held.
Poll sleeps on a private waiter registered with each object source; signal
interruption wakes the thread's current wait queue and needs no global poll
lock.
`proc_collect_pgrp` takes `proc_tree_lock` and then
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

Each `run_queue.lock` covers only one CPU's ready and sleeper lists.  A remote
wake uses that CPU's MPSC inbox and a reschedule IPI.  Stealing holds the
local lock and only tries a victim lock; it never waits for a second queue
lock.  `cpu.current`, `idle` and `zombie_pending` are local-queue state, while
`need_resched` is atomically set by the local tick or IPI.

`console_lock` is innermost because any subsystem may print while holding its
own lock. Code holding `console_lock` must not call into any other subsystem.
The panic path bypasses `console_lock` once `panic_in_progress` is set.
After `consoleout` starts, a sleeping `console_drain` mutex serializes the
single normal consumer with explicit flushes. The slow polled UART runs while
holding that mutex but no spinlock; `console_lock` is acquired separately for
the framebuffer state and framebuffer write.

## M23 additions

- `sock_table_lock` (listener names) -> `sock->lock` (a listener's
  backlog). `conn->lock` protects descriptor records and connection state;
  direction byte indexes are SPSC atomics.  Each side's poll source is
  notified after connection state changes.
- `timerfd_lock` protects the armed timer list and every timer's
  fields; it is taken from the timer interrupt and is the condition
  lock of the timers' wait queues.
- `timed_lock` (timed wait queue waiters) is taken after the caller's
  condition lock in `waitq_wait_timeout`, and before `wq->lock` inside
  `waitq_interrupt` from the timer interrupt.
- `eventfd->lock` is leaf.

## M32 additions

- M47: `input_dev.lock` protects one input device's key state, repeat
  state and reader queues; it is taken from interrupt handlers and
  virtqueue completions and nests above `waitq.lock` and
  `poll_source.lock`. `input_devices_lock` (the device list, condition
  lock of `repeat_waitq`) is taken before `input_dev.lock` by the repeat
  thread. The console keyboard state `kbd_lock` (`input/keyboard.c`) and
  `console_tty.lock` are taken only after `input_dev.lock` has been
  released. `mouse_lock` is now the PS/2 packet assembly only.
- `mouse_lock` (the `/dev/mouse` ring in `drivers/mouse.c`, until M47) is taken from
  the PS/2 interrupt handler after the driver's own packet lock, and from
  virtio-input completion callbacks under the event queue's `vq->lock`
  and the device's report lock: `ps2 mouse_lock -> mouse_lock`,
  `vq->lock -> input_dev->lock -> mouse_lock`. It is the condition lock of
  `mouse_waitq`; its object-local poll source is notified on publication.
- `fb_mode_lock` (mutex; the geometry of `fb_screen` for `/dev/fb0`
  readers and mode changes) is taken before the GPU driver's mutex and
  before `console_lock`: `fb_mode_lock -> virtio_gpu->lock -> vq->lock`
  and `fb_mode_lock -> console_lock`. `console_set_screen` takes only
  `console_lock`; the console never calls the GPU driver (it records a
  dirty rectangle that `gpu_flushd` fetches with `console_take_dirty`,
  releasing `console_lock` before touching the device).
- `virtio_gpu->lock` (mutex; the scanout resource ids and every control
  sequence) is taken before the control queue's `vq->lock`. The panic
  path skips the mutex and the flush entirely when the queue lock is held.
- `input_dev->lock` (the report assembled between `SYN_REPORT` events) is
  taken under `vq->lock` in the completion callback and alone by
  `virtio_input_feed`.

## M35 additions

- `futex_bucket.lock` (one per hash bucket of `ipc/futex.c`; the list of
  waiters keyed by process and address) is the condition lock of every
  waiter's private wait queue: `futex_bucket.lock -> waitq.lock ->` the
  calling CPU's `run_queue.lock` while blocking, and
  `futex_bucket.lock -> timed_lock` through
  `waitq_wait_timeout`. It is never taken from an interrupt. The RTC
  epoch offset is a single 64-bit word written by `rtc_init` and
  `clock_settime` and read by `clock_gettime`; it needs no lock.

## M36 additions

- `mfs_journal.lock` (spinlock; the counters and the pinned buffer list
  of one journal) is a condition lock: `mfs_journal.lock -> waitq.lock ->`
  the calling CPU's `run_queue.lock` while blocking. It is taken from
  `op_begin` and `op_end`, which the VFS
  calls with no inode or file lock held, and from `mfs_journal_write`
  under an inode mutex, `mfs_sb.lock` and a buffer mutex; it is never held
  across a device transfer. The commit itself runs with `committing` set
  and the spinlock released, taking one `buf.lock` at a time.
- `fat_sb.lock` (mutex; the allocation table) sits where `mfs_sb.lock`
  does: under an inode mutex, above `buf.lock`, because cluster
  allocation happens inside a write and reads and writes table entries
  through the cache.

## M37 additions

- `mapping.lock` (mutex) sits between `file.lock` and `inode.lock`:
  `file_read` and `file_write` hold `file.lock` and take it to overlay or
  copy through cached pages, and a page fill or writeback takes it and then
  calls the filesystem's read or write operation, which takes `inode.lock`.
  `filemap_writeback` calls `vfs_op_begin` before taking it, as the VFS
  does for writes. The fault handler and the unmap paths never hold a
  `vmspace.lock` while taking it: `filemap_fault` releases the space lock
  first and the unmap and msync paths queue their writebacks and run them
  after the space lock is released.
- `mapping.dirty_lock` is a leaf taken under `vmspace.lock` (gathering
  hardware dirty bits) and under `mapping.lock` (writeback).
- `filemap_lock` is a leaf in level 16; it is taken while a `vmspace.lock`
  is held only through `filemap_ref`, which is atomic and takes no lock.

## M40 additions

- No new lock. `proc.rlim` is written under `proc.lock` and read without
  it by the timer tick and the enforcement points; the CPU time, fault and
  switch counters of a process are updated atomically from the tick, the
  fault handler and `sched_switch_locked` (which holds the calling CPU's
  `run_queue.lock`). The
  tick calls `signal_send` for `RLIMIT_CPU`, taking `proc.lock` from the
  timer interrupt like `waitq_interrupt` already did; no spinlock is held
  when an interrupt arrives, so the ordering is unaffected.

## M41 additions

- `prof_lock` protects start, stop and close.  Sampling uses a per-CPU SPSC
  ring and per-CPU active count without this lock; `/dev/profile` merges the
  consumer sides.  Frame-chain translation still takes the interrupted
  process's `vmspace.lock` with no other lock held.

## M42 additions

- `CONFIG_LOCKSTAT` adds counters to every spinlock; the rows of the
  statistics table are appended under a raw `xchg` word (`table_lock` in
  `spinlock.c`) that is not itself a `struct spinlock`, taken with
  interrupts already disabled inside the acquiring lock's critical
  section, so it nests below everything and counts nothing.
- `proc_format_table` no longer takes `vmspace.lock` under
  `proc_list_lock`, which the order forbids (level 16 above level 11). It
  snapshots the rows under both process locks, then counts resident pages
  under `proc_tree_lock` alone, which is above `vmspace.lock`.

## M43-M46 additions

- `poll_source.lock` is local to one pollable object. Notification takes it
  before each private poll waiter lock; waking may then take that waiter's
  `waitq.lock`, but runnable publication uses the destination CPU's MPSC inbox
  and does not acquire a remote run-queue lock.
- RCU readers and callbacks add no lock-order level. Readers only disable
  preemption by entering a per-CPU read section and are forbidden to block.
  Callback producers publish through a per-CPU MPSC list; a dedicated kernel
  thread consumes all lists after every started CPU has crossed the target
  epoch. It holds no RCU lock across callbacks, which may acquire filesystem
  locks and sleep. Callbacks must never execute in the timer interrupt.
- Each scheduler path may block while holding only its own `run_queue.lock`.
  Work stealing uses `spin_try_lock` for a victim and skips that victim on
  failure, so it never waits while holding two run-queue locks.
- The local slab fast path takes `slab_magazine.lock`. Refill and drain then
  take the corresponding `kmem_cache.lock`; allocation of backing pages may
  continue to `pmm_lock`. `slab_reclaim` is an externally serialized
  maintenance operation and must not race cache creation or destruction.
- `pmm_cpu_cache` is taken alone on the order-zero fast path. Refill and
  reclaim release it before taking `pmm_lock`, so the two allocator locks do
  not nest. Statistics briefly take each CPU-cache lock after releasing
  `pmm_lock`.

## N01 additions

- `socket.lock` is a leaf protecting the error word of a socket; it is
  never held across a backend call. `families_lock` and
  `inet_protocols_lock` are leaves over registration tables.
- The Unix backend's `conn.lock` is now taken before `poll_source.lock`:
  readiness changes are announced while it is held, and a side that
  releases clears its pointer to the socket's poll source under it, so
  the source of a freed socket is never notified. The order below it is
  `conn.lock -> poll_source.lock -> poll_waiter.lock -> waitq.lock`,
  the one `input_dev.lock` already uses. `sock_table_lock ->
  unix_sock.lock` (the listener's backlog) is unchanged and never nests
  with `conn.lock`.
- `file.lock` is not taken for objects whose operations carry
  `FOPS_STREAM` (sockets): they have no position, and a reader blocked in
  the backend must not exclude a writer on the same open file
  description. Regular files keep the mutex.

## N02 additions

- `net_worker.lock` is the condition lock of the worker's sleep and of
  request completion: `net_worker.lock -> waitq.lock ->` the calling
  CPU's `run_queue.lock`, and `net_worker.lock -> timed_lock` through
  `waitq_wait_timeout`. It is never held while a packet, a request or a
  timer function runs, and not held across device processing. N03 extends its producer
  use to an IRQ-side kick/wakeup after recording queue completions, as
  described below.
- `pbuf_pool.lock` and `netif_lock` are leaves in level 16. The pool
  lock is taken by `pbuf_alloc` and `pbuf_free` from thread context;
  `net_worker_queue_packet` takes `net_worker.lock` after the ownership
  hand-over, which uses an atomic word and no lock. The interface flag is
  read with an acquire load on the data paths without `netif_lock`.

## Lua workers and libc stream registration

These locks are in user space and do not add a kernel lock-order level.

- `streams_lock` in `libc/src/stdio/stdio.c` protects the open-stream registry
  and the lifetime of entries visited by `fflush(NULL)`. Ordering is
  `streams_lock -> FILE.lock`. `fclose` removes the entry before acquiring
  its FILE lock for the final flush. Registration allocates before locking.
- `job.lock` in `user/lua/lthread.c` protects a worker's message queues, their
  notification pipes, cancellation and completion state. Message allocations,
  Lua operations and callbacks occur outside this mutex. Pipe operations
  under it are nonblocking. No job lock nests inside another job lock.
  Reference counts and the process-wide active worker count use atomics.
  Worker errors become visible through completion and pthread join.

## N03–N05 additions

- The VirtIO device `irq_lock` serializes interrupt traversal with queue
  detachment after reset: `irq_lock -> virtqueue.lock -> net_worker.lock
  -> waitq.lock`. Completion callbacks only record tokens and kick netd;
  they never allocate, free packets, refill descriptors or parse frames.
  This extends N02: the worker condition lock may be taken in an interrupt
  solely to record a kick and wake its waiter. Netd holds none of its locks
  while servicing the NIC. The NIC's DMA buffers and completion slots are
  protected by the corresponding queue lock.
- IPv4 configuration, routes, ARP entries and deadlines belong exclusively
  to netd. Configuration and socket sends use bounded synchronous requests
  holding kernel copies; file references keep socket objects alive until
  completion. Close uses an uninterruptible request to remove its endpoint.
- `udp_lock` protects the bounded endpoint table, bindings, peer selection
  and receive rings. Its order is `udp_lock -> socket.lock` and
  `udp_lock -> poll_source.lock -> poll_waiter.lock -> waitq.lock`.
  A receiver sleeps with udp_lock as condition lock. All copies under it
  are bounded copies between kernel buffers. Packet freeing and output
  happen after unlocking. Netd never waits for a reader or a descriptor.

## N06 TCP ownership

- Netd exclusively owns TCP connection state, sequence numbers, listener
  membership and deadlines. A connection control block may outlive its socket
  through FIN processing and TIME_WAIT. The fixed connection table is distinct
  from the application endpoint table.
- `tcp_lock` protects endpoint publication, application-visible readiness,
  address snapshots, receive rings and wait conditions. The order is
  `tcp_lock -> socket.lock` and `tcp_lock -> poll_source.lock ->
  poll_waiter.lock -> waitq.lock`. No packet output, allocation, user copy or
  worker request submission occurs while tcp_lock is held.
- Socket operations submit copied, bounded requests while their file reference
  keeps the endpoint alive. Close removes the endpoint with an uninterruptible
  worker barrier; remaining protocol state then contains no socket pointer.
  A blocked connect interrupted by a signal leaves the connection in progress;
  a later poll/SO_ERROR or close observes/terminates it. Accept waiters register
  under the same condition lock used when publishing accepted connections.

## N07–N09 additions

- `tcp_lock` additionally protects the out-of-order presence bitmap,
  `out_of_order` and the pending FIN of a connection's receive store, since
  `tcp_receive_segment` on netd and `tcp_receive` on a reader share the ring.
  The send buffer, congestion state, round-trip estimator and every deadline
  belong to netd alone and are never read under `tcp_lock`; readiness is
  published as before through `tcp_publish`.
- Reassembly contexts, the path MTU cache and the recent-transmission table
  (`fragment.c`, `path.c`) belong to netd. `netif_set_up(false)` from another
  thread drains the worker and then runs a request that fails the interface's
  connections, flushes its reassembly contexts, the path tables and its ARP
  entries; called on netd it runs the same function directly. No lock is held
  across that request.
- `random_lock` is a leaf in level 16 protecting the generator state of
  `kernel/lib/random.c`. It is taken from thread context by `random_u32`,
  including on netd under no other lock, never from an interrupt. Readiness
  is an acquire/release flag set once by `random_init`.
- The entropy device's `virtqueue.lock` is used only during `random_init`,
  before netd services any interface; its completion callback records the
  length and never allocates. The device is reset before its buffer is freed.

## N10–N11 additions

- `icmp_lock` is a leaf in level 16 protecting the four echo request slots
  of `icmp.c`. The caller of `icmp_echo` sleeps on the slot's wait queue
  with `icmp_lock` as condition lock and a deadline; netd completes a slot
  from echo-reply or ICMP-error input under the same lock and never sleeps.
  The request that emits the echo runs on netd without the lock.
- `/dev/net` reads take a snapshot through a worker request; the counters
  are netd-owned and the text is assembled on netd. The UDP `broadcast`
  flag is protected by `udp_lock` like the other endpoint fields.

## N13–N16 additions

- The send store, receive store and presence bitmap of a TCP connection
  are allocated by netd with no lock held when the connection is created
  and freed by netd. The receive pointers are set before any endpoint can
  reach the connection and are cleared only when no endpoint exists, so a
  reader that finds the connection under `tcp_lock` always finds its
  store. `tcp_release_receive` clears the counts under `tcp_lock` and frees
  the memory after releasing it; no allocation or free happens under
  `tcp_lock`.
- The SACK report list, the scoreboard, the recovery state and the
  delayed-ACK deadline of a connection belong to netd, like the rest of its
  sending state, and are never read under `tcp_lock`. Finding the run of
  stored bytes for a SACK block reads the presence bitmap, so it runs under
  `tcp_lock` inside `tcp_receive_segment`; the report list is updated after
  the lock is released.
