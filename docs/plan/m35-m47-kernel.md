# Milestones M35 to M47: threads, storage, memory, accounting and scalability

## M35. POSIX threads and time (completed 2026-09-03)

1. Thread local storage: the kernel keeps an FS base per thread
   (`set_tls`, loaded at every switch and first run, inherited by fork and
   raw threads, cleared by exec) and adds `gettid`. libc gives every thread
   a control block at its FS base; `errno` lives there.
2. `futex` (`kernel/ipc/futex.c`): private wait and wake on a word of the
   process, with timeouts and signal interruption, hashed into buckets
   with per-waiter wait queues.
3. `pthread.h` in libc: threads with mmap'd stacks, join, detach (reclaimed
   by later calls), attributes, three state futex mutexes (normal,
   recursive, error checking, timed), sequence based condition variables
   with timed waits, keys with destructors, once, spin locks, read-write
   locks. `malloc`, every `FILE` and `atexit` are locked.
4. The CMOS real time clock read at boot (`drivers/rtc.c`), `clock_gettime`
   and `clock_settime` with `CLOCK_REALTIME` and `CLOCK_MONOTONIC` at
   nanosecond scale (`timer_ns`), and `time.h`/`sys/time.h` in libc:
   `time`, `clock`, `nanosleep`, `gettimeofday`, `gmtime`, `mktime`,
   `strftime`, `asctime`, `ctime`. `date` and `cal` in coreutils; the panel
   and the clock application show the time of day.
   `docs/design/threads.md` and `docs/design/time.md` describe it.

Tests: `pthreads` (contention, producers and consumers, mutex types, timed
waits, per-thread errno, keys, once, detached threads, malloc and stdio
from several threads, spin and read-write locks) and `time` (both clocks,
sleeping, calendar round trips, formatting, setting the clock, the RTC
boot line).

## M36. mfs journaling, fsck and FAT (completed 2026-09-04)

1. mfs format version 2 places a journal of 128 blocks between the inode
   table and the data blocks (`kernel/include/fs/mfs_format.h`; `mkfs`
   writes it, `--dump` shows its state). The kernel journals metadata
   (`kernel/fs/mfs/journal.c`): every modifying VFS operation runs between
   the new superblock hooks `op_begin` and `op_end`, metadata buffers are
   pinned in the block cache, and the last operation of a group commits:
   dirty data first, then the pinned blocks to the journal slots, then the
   header with count and CRC-32, then the blocks to their home locations.
   A mount replays a committed transaction and discards an incomplete one.
   Test hooks simulate a crash before and after the commit.
2. `tools/fsck` (`build/host/fsck`): replays the journal, then checks
   inodes and block pointers, the directory tree, connectivity (with
   `lost+found`), the bitmaps and the counters; `-y` repairs, `-n` never
   writes; exit status 0, 1 (repaired), 4 (problems left) or 8.
3. FAT12, FAT16 and FAT32 in `kernel/fs/fat/`: reading and writing with
   long file names, case insensitive lookup, create, mkdir, unlink, rmdir,
   rename, truncate, timestamps from the real time clock, the FAT32 FSInfo
   sector. `tools/mkfat` builds images of any of the three types from a
   directory tree and inspects them (`--dump`, `--cat`).
4. The test runner attaches extra devices: `mfs2` (an empty mfs image) and
   `fat` (one image per line, built by mkfat from a directory under the
   case); the post script sees them as `DISK2` and `FATIMGS`.
   `docs/design/mfs.md` and `docs/design/fat.md` describe it.

Tests: `mfs_journal` (crash after and before a commit on a second image,
concurrent operations sharing transactions, bitmap and counter agreement,
a committed transaction left on the root image and replayed by `fsck` in
the post script), `fat` (the same tree on all three types: long names,
case folding, writes of 100000 bytes, short name uniqueness, renames,
truncation, unlink of an open file, cluster accounting across a remount,
the result read by `mkfat` on the host) and `fat_user` (`/bin/fattest`
through `mount`, stdio and `dirent`).

## M37. File backed mappings and mprotect (completed 2026-09-04)

1. `mmap` of regular files (`kernel/mm/filemap.c`): a region carries the
   open file, its offset and a reference on the inode's `struct mapping`,
   the cache of file pages that are mapped somewhere. A fault reads the
   page through the file's read operation into a fresh frame and maps it;
   `MAP_SHARED` maps the cached frame itself, so every mapper of the file
   sees the same memory, `MAP_PRIVATE` maps it read only with copy on
   write. Pages beyond the end of the file fault (`SIGSEGV`). Hardware
   dirty bits are gathered into the mapping's dirty bitmap when a shared
   region is unmapped or synced, and dirty pages are written back through
   the file; the cache is dropped with the last mapper, so a file that is
   not mapped costs nothing. `read` overlays the cached pages and `write`
   copies through them, so ordinary I/O and mappings stay coherent.
   `msync` writes the dirty pages of a range.
2. `mprotect` splits regions at the boundaries and rewrites present
   entries; copy on write frames keep their protection. `PROT_NONE`
   entries keep their frame behind the software bit `PTE_PROTNONE`.
   `MAP_FIXED` replaces whatever the range held.
3. libc: `mprotect`, `msync`, `MAP_SHARED`, `MAP_FIXED`, `MS_*`.
   `docs/design/filemap.md` describes it.

Tests: `mmap_file` (`/bin/mmapfiletest`: private and shared mappings of a
file on the root disk, offsets, aliasing inside one process and across
fork, coherence with `read` and `write`, writeback through `msync` and
through the last `munmap`, private copies that never reach the file, a
fault beyond the end of the file, `mprotect` splitting and `PROT_NONE`,
`MAP_FIXED`, and the leak check of the `run` harness).

## M38. madvise (completed 2026-09-04)

1. `madvise` (`kernel/mm/madvise.c`): `MADV_NORMAL`, `MADV_RANDOM` and
   `MADV_SEQUENTIAL` are recorded, `MADV_WILLNEED` populates a range
   (zero pages or file pages), `MADV_DONTNEED` drops the frames and swap
   slots of a range so the next touch yields zero pages or file contents
   again, `MADV_FREE` clears the dirty bits and tags the entries with
   `PTE_LAZYFREE`: kswapd frees such a frame instead of swapping it when
   the dirty bit is still clear, and a page written again in the meantime
   is kept. `MADV_DONTFORK` and `MADV_DOFORK` set and clear `VM_DONTFORK`,
   which `vmspace_fork` skips. `MADV_HUGEPAGE` and `MADV_NOHUGEPAGE` set
   the flag M39 acts on. Regions are split as needed.
2. `/dev/meminfo` gains `LazyFreed:`, the count of frames reclaimed
   through `MADV_FREE`. `docs/design/madvise.md` describes it.

Tests: `madvise` (`/bin/madvisetest`: every advice on anonymous and file
regions, `DONTNEED` on a swapped range, `FREE` followed by a rewrite that
must survive kswapd, `FREE` on an untouched range that kswapd reclaims
under memory pressure, `DONTFORK` regions absent in the child, errors).

## M39. Huge pages (completed 2026-09-04)

1. Anonymous regions flagged `VM_HUGE` (`MAP_HUGETLB`, or `MADV_HUGEPAGE`
   on a region) are backed by 2 MiB frames from the buddy allocator
   (order 9) wherever a 2 MiB aligned block lies entirely inside the
   region and no 4 KiB table exists there yet (`kernel/mm/huge.c`). The
   frame is mapped by a page directory entry with `PTE_PS`; fork shares
   it copy on write like a small page and a write fault copies the whole
   2 MiB. `munmap`, `mprotect` and `madvise` that cut through a huge page
   split it first: a private block is broken into 512 page table entries
   with one reference per frame (`pmm_split_block`), a shared block is
   copied. kswapd, `swap_in_all` and the kernel's user accessors skip or
   handle level 2 entries. When no 2 MiB frame is available the fault
   falls back to 4 KiB pages.
2. `/dev/meminfo` reports `HugePages:` (mapped 2 MiB frames) and
   `HugeSplits:`. `docs/design/hugepages.md` describes it.

Tests: `hugepages` (`/bin/hugetest`: `MAP_HUGETLB` alignment and the
count in meminfo, `MADV_HUGEPAGE` on an aligned anonymous region, data
integrity across fork and copy on write of a huge page, partial `munmap`
and `mprotect` splitting with the remaining data intact, fallback when
the buddy allocator has no order 9 block, and no leak).

## M40. Per process resource limits and CPU accounting (completed 2026-09-04)

1. Every process carries `struct rlimit rlim[RLIMIT_NLIMITS]`, inherited
   by fork, kept by exec, read and written with `getrlimit`, `setrlimit`
   and `prlimit` (any pid). Enforced: `RLIMIT_AS` (sum of region sizes,
   checked by `mmap`, `sbrk` and thread stacks), `RLIMIT_DATA` (heap
   size), `RLIMIT_STACK` (size of the main stack region set up by exec,
   between 64 KiB and 1 GiB), `RLIMIT_NOFILE` (descriptor slots, at most
   `OPEN_MAX`), `RLIMIT_NPROC` (user processes, `fork` fails with
   `EAGAIN`), `RLIMIT_FSIZE` (`write` stops at the limit with `EFBIG` and
   `SIGXFSZ`), `RLIMIT_CPU` (`SIGXCPU` at the soft limit, `SIGKILL` at the
   hard one). `RLIMIT_CORE`, `RLIMIT_RSS`, `RLIMIT_MEMLOCK` are stored
   only.
2. CPU time: the timer tick charges the running thread's process with a
   user or system tick (`proc_account_tick`), summed into the parent's
   children totals when a child is reaped. `getrusage` (`RUSAGE_SELF`,
   `RUSAGE_CHILDREN`, `RUSAGE_THREAD`) reports user and system time, page
   faults (`minflt`), resident pages (`maxrss`, counted on demand) and
   context switches; `wait4` fills its `rusage` argument. `/dev/proc`
   gains `TIME` (ticks) and `RSS` (KiB) columns, `ps` prints them.
3. `ulimit` builtin in the shell, `prlimit` and `time` in coreutils.
   libc: `sys/resource.h`. `docs/design/rlimit.md` describes it.

Tests: `rlimit` (`/bin/rlimittest`: get, set, inheritance, the hard limit
ceiling, `EINVAL` and `EPERM` cases, `RLIMIT_AS` refusing `mmap` and
`sbrk`, `RLIMIT_DATA`, `RLIMIT_NOFILE` refusing `open` and `dup`,
`RLIMIT_NPROC` refusing `fork`, `RLIMIT_FSIZE` with `SIGXFSZ`,
`RLIMIT_STACK` visible as the main stack size of an exec'd child,
`RLIMIT_CPU` killing a spinning child with `SIGXCPU` then `SIGKILL`,
`getrusage` times growing under load, `prlimit` on another pid).

## M41. Sampling profiler (completed 2026-09-04)

1. `kernel/debug/profile.c`: the timer interrupt of every CPU records the
   interrupted instruction pointer, up to 8 frame pointer chained return
   addresses (walked through the page tables so a bad pointer never
   faults), pid, tid, CPU and a user/kernel flag into a ring of samples
   when profiling is on for that process (or all). `/dev/profile` starts
   and stops sampling with `ioctl` (`PROF_START` for a pid or 0, `PROF_STOP`,
   `PROF_SET_DIVIDER` for the sampling period in ticks), `read` returns
   whole `struct prof_sample` records and `poll` reports data. Dropped
   samples are counted. `/dev/ksyms` lists the kernel symbol table as
   `addr size name` lines so user space can symbolize kernel addresses.
2. libc `minios/profile.h`: the ring reader and a symbolizer that loads
   the `.symtab` of a static ELF (`/bin/<name>`) or `/dev/ksyms` and
   aggregates samples by symbol, with or without call chains.
3. `prof` in coreutils: `prof [-d seconds] [-k] [-c] command args...` or
   `prof -p pid`, printing the flat profile (percent, samples, symbol,
   binary or kernel) and optionally the top call chains.
4. `sysmon` gains a CPU column computed from the `TIME` ticks of
   `/dev/proc`, a memory column from `RSS`, and a Profile tab: profiling
   the selected process (or the whole system) shows the live top symbols
   refreshed every second, with a kernel/user toggle.
   `docs/design/profile.md` describes it.

Tests: `profile` (`/bin/proftest`: a hot function dominating the user
samples of its own process, samples attributed to a child pid and to
kernel symbols, call chains reaching `main`, filtering by pid, the
divider, stop and restart, and ring overflow accounting) and the
existing `gui_tools` case with sysmon.

## M42. Lock statistics and lock aware profiling (completed 2026-09-04)

1. `CONFIG_LOCKSTAT` (`kernel/sync/spinlock.c`, `kernel/sync/lockstat.c`):
   every spinlock counts acquisitions, contended acquisitions, cycles
   spent spinning, cycles held and the longest hold with its acquirer,
   aggregated by lock name; `/dev/lockstat` prints the table and a write
   resets it.
2. The profiler attributes kernel samples taken with interrupts disabled
   to the code that held the lock (`prof_attribute` in libc, marked
   `(locked)` in `prof` and sysmon); `prof -a` samples every process with
   a per process table.
3. `/dev/proc` no longer walks page tables under `proc_list_lock`.
   `docs/design/lockstat.md` holds the measurements of the desktop and
   `docs/design/lockfree.md` the design and plan for M43 to M46.

Tests: `prof_gui` (the desktop headless with a terminal, `mandel`,
sysmon and the clock, pointer motion, `prof -k -c -a` for ten seconds and
the lock table on the serial log).

## M43. Lock-free system call paths (completed 2026-09-05)

1. `kernel/sync/atomic.h`, `percpu.h`, `rcu.h`, `mpsc.h`: reference counts
   with `refcount_inc_not_zero`, per CPU counters, RCU for the
   non-preemptible kernel (grace periods counted at context switches and
   in the idle loop, deferred frees on a per CPU MPSC list reclaimed from
   the tick) and the MPSC stack it needs.
2. The signal and exit checks on system call entry and exit read atomic
   words and take `proc.lock` only when a signal is pending.
3. `struct file` reference counts are atomic and `files_lock` is removed;
   `fdtable_get` looks descriptors up without the table lock through an
   acquire load and `refcount_inc_not_zero`, closed files are freed
   through RCU.
4. `user_range_ok` walks an RCU protected region list without
   `vmspace.lock`; if time allows, `copy_from_user` and `copy_to_user`
   with a fault fixup table replace the walk.
5. `poll` registers on a wait queue per object and producers wake only
   their own queue; `poll_notify`, `poll_lock` and the global generation
   are removed; readiness checks read atomic counts.
   `docs/design/lockfree.md` gains the implemented details.

Tests: `lockfree` (the primitives on four CPUs: counters, refcounts,
RCU readers against a writer freeing nodes, the MPSC stack), `poll_wake`
(one producer wakes only its own consumer, measured with the switch
counters of `getrusage`), and `prof_gui` with the `proc`, `files_lock`
and `poll_lock` rows gone.

## M44. Per CPU scheduler (completed 2026-09-05)

1. One run queue lock per CPU; wakeups for another CPU go through its
   MPSC list and a reschedule IPI; stealing takes the victim's lock only.
2. The tick accounts the running thread's slice without a lock; sleepers
   and the boost are per CPU.
3. `docs/design/sched.md` and `locking.md` describe the new ordering.

Tests: `sched`, `smp`, `smp_user`, `pthreads` unchanged, plus `prof_gui`
with no `sched_lock` row and run queue contention under one percent.

## M45. Lock-free byte streams and the console (completed 2026-09-05)

1. `kernel/sync/ring.h`: the single producer, single consumer ring used
   by pipes, pseudo terminals, the console tty, the socket data path, the
   kernel log, the mouse and keyboard event rings and one profiler ring
   per CPU.
2. `console_write` appends to the log ring and a console thread drives
   the UART and the framebuffer console; the panic path writes directly.

Tests: `pipes`, `pty`, `sockets`, `kbd`, `mouse`, `profile` unchanged,
`ring` (producer and consumer on different CPUs, wrap around, full and
empty transitions), and `prof_gui` with the `console` row under one
millisecond of hold time.

## M46. Per CPU allocators and address space counters (completed 2026-09-05)

1. Per CPU magazines in front of the slab caches and per CPU single page
   lists in front of the buddy allocator; reported allocator totals include
   objects and pages held in those per CPU caches.
2. `vma_populate` zeroes pages before taking the space lock; a per CPU
   resident counter per address space replaces `vma_count_resident`;
   `munmap` batches its TLB shootdowns.

Tests: `slab`, `pmm`, `vmm`, `swap`, `hugepages` unchanged, `prof_gui`
with `kmalloc-*` and `pmm_lock` rows near zero for the steady state and
`vmspace` holds under 100 microseconds.

## M47. Input subsystem (completed 2026-09-05)

1. An input core (`kernel/input/`): drivers register `struct input_dev`
   with capabilities and report evdev style events (Linux key codes,
   `REL_*`, `ABS_*`, `SYN_REPORT`) with microsecond timestamps. The core
   keeps the keys down per device, drops presses of keys already down,
   repeats the held key in software, delivers to `/dev/input/eventN`
   readers (queues of 1024 events, `SYN_DROPPED` on overflow, `EVIOCGRAB`,
   `EVIOCGCAPS`, `EVIOCGKEY`, `EVIOCGABS`, `EVIOCGREP`/`EVIOCSREP`) and
   feeds ungrabbed keyboards to the console terminal through
   `input/keyboard.c`, which counts both keys of every modifier pair.
   `/dev/mouse`, `/dev/kbd` and `KBD_SCANCODES` are gone; devfs has one
   level of directories.
2. The PS/2 keyboard translates scancode set 1 to key codes; the PS/2
   mouse and virtio-input (now including keyboards, attached by
   `tools/run.sh` as `virtio-keyboard-pci`) report to the core.
3. The compositor reads and grabs every `/dev/input` device; the cursor
   is kept in fractions of a pixel with an acceleration profile
   (`pointer_speed`, `pointer_accel` flat or adaptive, in the settings
   program's Mouse page); motion and wheel values are fixed point.
4. libgui repeats a held key from `repeat_info`; key codes in the
   toolkit and the applications are the `KEY_*` names.

Tests: `input`, `mouse`, `mouse_wheel`, `kbd`, `input_tablet`,
`input_keyboard`, `gui_kbd_restore`, `gui_pointer`, `gui_repeat`,
`gui_tools` (no repeat for a released key), `comp_seat` and the GUI
cases, which place the cursor through the virtual tablet.
`docs/design/input.md`.
