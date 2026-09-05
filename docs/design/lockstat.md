# Lock statistics and the lock hot paths (M42)

This document records the M42 baseline that motivated M43-M46. Names such as
`files_lock`, `poll_lock` and `sched_lock` in the measurements and findings
describe that baseline; the later implementation removes them as documented
in `lockfree.md` and `sched.md`.

## Instrumentation

`CONFIG_LOCKSTAT` (default 1, `toolchain.mk`) adds two words to every
spinlock: a pointer to a `struct lockstat` row and the TSC value at the
current acquisition. Rows are aggregated by lock name, so the one lock of
every address space, file or inode shows up as a single row, and a lock
that is freed leaves nothing dangling. `spin_lock` and `spin_lock_irqsave`
count acquisitions, count contended acquisitions (the first `xchg`
failed) and add the cycles spent spinning; `spin_unlock` adds the cycles
held and records the longest hold with the acquirer's return address
(`CONFIG_LOCKDEBUG` supplies it). The table has 128 rows and is append
only; a row is looked up once per lock and then cached in the lock.

`/dev/lockstat` prints the table as text (`NAME ACQUIRES CONTENDED
SPIN_CYCLES HOLD_CYCLES MAX_HOLD MAX_HOLD_CALLER`); writing anything to it
resets the counters. With the TSC at about 1 GHz under QEMU on this host,
cycles are nanoseconds.

The profiler attributes kernel samples to the code that held a lock:
a sample whose innermost frames are `pop_cli`, `push_cli`, `spin_lock`,
`spin_unlock` or their `irqsave` variants is charged to the first frame
outside them, marked `(locked)` (`prof_attribute` in libc, used by `prof`
and by the sysmon Profile tab). The timer interrupt cannot fire while
interrupts are disabled, so it fires on the `sti` in `pop_cli` when it was
pending; `pop_cli` at the top of a flat profile means time spent inside
spinlock critical sections. `prof -a` samples every process, prints a
per process table and symbolizes each sample with the binary of its own
process.

`tests/cases/prof_gui` runs the desktop (X12, panel, sysmon, clock, a
terminal running `yes`, `mandel`) headless, moves the pointer, samples
every process for ten seconds with `prof -k -c -a` and prints
`/dev/lockstat`; the output on the serial log is the data below.

## Measurements

The screenshot of the sysmon Profile tab on the real desktop (four CPUs,
2560x1600 at scale 2) showed 1253 samples: 850 user, 403 kernel, and
`pop_cli` held 291 of the kernel samples (72 percent). The kernel time of
an idle desktop is therefore spent almost entirely inside spinlock
critical sections, and the user time is painting (`painter_blit`,
`csd_copy`, `memcpy`, `draw_surface`).

The headless scenario reproduces the kernel side. Over about eleven
seconds with one client (`mandel`, whose 1 ms repeating timer makes
`app_step` call `poll` about 155,000 times per second):

| Lock | Acquisitions | Contended | Held (ms) | Longest hold | Longest holder |
|---|---|---|---|---|---|
| `proc` | 11,131,915 | 0 | 537 | 55 us | `signal_deliver` |
| `fdtable` | 1,862,418 | 0 | 380 | 423 us | `fdtable_get` |
| `files_lock` | 3,723,537 | 4,054 | 205 | 35 us | `fdtable_get` |
| `vmspace` | 1,861,974 | 185 | 192 | 5,472 us | `vma_populate` |
| `sockconn` | 1,871,846 | 46 | 180 | 47 us | `sock_poll` |
| `poll_lock` | 1,878,672 | 8,621 | 123 | 43 us | `poll_files` |
| `sched_lock` | 48,794 | 5,266 | 71 (+26 spinning) | 80 us | `waitq_wait` |
| `console` | 256 | 0 | 46 | 543 us | (irqsave, no caller) |
| `proc_tree_lock`, `proc_list_lock` | 243 | 0 | 22 each | 157 us | `proc_format_table` |
| `mouse_lock` | 6,681 | 14 | 9 | 75 us | `ps2mouse_feed_byte` |
| `kmalloc-32` | 1,006 | 0 | 7 | 353 us | `kmem_cache_alloc` |

The sampled profile of the same run had 79 of about 100 kernel samples
inside spinlock sections, the same share as the screenshot.

## Findings

One `poll` system call on a single socket descriptor takes twelve
spinlocks, each with `push_cli`/`pop_cli`: `proc.lock` six times
(`signal_should_interrupt` inside `poll_files`, `proc_exit_check` and
`signal_deliver` on the way back to user mode, and the same checks on
the entry path), `vmspace.lock` once (`user_range_ok` walks the region
list for the `pollfd` array), `fdtable.lock` once, the global
`files_lock` twice (`file_ref` in `fdtable_get`, `file_put`), the
socket's `conn.lock` once (`sock_poll`) and the global `poll_lock` once.
The first six of those protect data that is read far more often than it
is written and can be read without a lock.

`poll_lock` and `poll_waitq` form a single wait queue that every producer
wakes (`poll_notify`): every socket write, pipe write, timer or input
event wakes every process sleeping in `poll`, and each of them rescans
all of its descriptors and goes back to sleep. On the desktop every
client is such a sleeper. This is the thundering herd, and it is also why
`poll_lock` is the most contended lock (8,621 contended acquisitions).

`sched_lock` is a single lock for the run queues of all CPUs. It is
taken 4,000 times per second by the timer ticks alone (every CPU,
`sched_tick_cpu`, to decrement the running thread's slice) and by every
wakeup and switch; 10.8 percent of its acquisitions found it taken.

`console_lock` is held for up to 543 us per write with interrupts
disabled, because `console_write` drives the polled UART and the
framebuffer console under the lock. X12 logs every commit and pointer
event to the console, so this is a steady cost on the desktop.

`vmspace.lock` is held for 5.5 ms by `vma_populate` when exec zeroes the
stack pages under the lock, and `kmalloc-32` for 353 us when a slab grows
(page allocation, redzone and poison writes under the cache lock).

`proc_format_table` (M40) walked page tables under `proc_tree_lock` and
`proc_list_lock`, 157 us per read of `/dev/proc`, and took `vmspace.lock`
under a leaf lock against the recorded order; this branch moves the walk
outside `proc_list_lock` and keeps only `proc_tree_lock`, which is above
`vmspace.lock`, while a process's size is counted. The complete fix is
the resident counter of M46.

On the application side, `app_step` in libgui allocates and frees the
`pollfd` array on every iteration and `mandel` drives its rendering with
a 1 ms timer, so a client can turn into a system call loop; the kernel
cost per call is what the following milestones reduce, the client
behaviour is noted for libgui.

The plan that follows from these numbers is in `lockfree.md`.

## M43-M46 validation

A controlled four-CPU TCG run of `tests/cases/prof_gui` on 2026-09-05 after
M46 produced the following relevant rows. The removed `files_lock`,
`poll_lock` and `sched_lock` names were absent.

| Lock | Acquisitions | Contended | Longest hold |
|---|---:|---:|---:|
| `run_queue` | 50,223 | 3 (0.006%) | 96 us |
| `console` | 317 | 0 | 399 us |
| `console_drain` | 11,079 | 0 | 15 us |
| `pmm_lock` | 3,503 | 33 | 24 us |
| `pmm_cpu_cache` | 832 | 0 | 3 us |
| `slab_magazine` | 17,564 | 0 | 29 us |

The UART work is serialized by the sleeping console drain mutex, so the
`console_drain` row measures only its internal condition spinlock; the slow
polled serial write does not hold a spinlock. `vmspace` had no acquisition
after the test reset the counters: `vma_populate` no longer allocates or
zeroes data or page-table frames under the space lock, and whole-space
teardown relies on the mandatory drop performed by `vmspace_destroy`.
