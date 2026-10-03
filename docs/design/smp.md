# Symmetric multiprocessing (M18)

The kernel runs on every processor QEMU provides (`-smp N`, `QEMU_SMP` or
`--smp` for `make run`, the `cpus` file of a test case, default 4). All CPUs execute
the same kernel with the same page tables. Each CPU has a `struct cpu`
reached through the GS base, its own idle thread, kernel stack, GDT, TSS,
local APIC timer and run queues. Shared structures retain the locks they
have carried since their introduction. M18 added `tlb_lock`; M44 later
replaced the scheduler's global lock with one run-queue lock and one
remote-wake inbox per CPU.

## Processor startup

Application processors are enumerated and started through the Limine MP
protocol (`arch/x86_64/smp.c`). Limine leaves every AP parked in long mode
on its own page tables and stack, both in bootloader reclaimable memory,
and jumps to `goto_address` when the kernel writes it. The kernel therefore
starts the APs in two stages:

- `smp_park_aps` runs after `vmm_init` and before `pmm_reclaim_bootloader`.
  For each AP it allocates a kernel stack (`kstack_alloc`), stores the
  `struct cpu` pointer in `extra_argument` and writes `goto_address`. The
  AP enters `ap_entry`, loads the kernel `CR3`, sets its GS base, switches
  to the kernel stack, loads its GDT, TSS and the shared IDT, then sets
  `cpu.online` and spins. From that point no AP references bootloader
  memory, so the boot CPU may reclaim it.
- `smp_start_aps` runs after `sched_init` and `sti` on the boot CPU. The
  APs leave their spin loop, enable NX, PAT, `CR0.WP` and `CR4.PGE`,
  enable their local APIC, program the `syscall` MSRs, start their timer,
  create their idle thread (`sched_init_cpu`), set `cpu.started` and enter
  `sched_idle_loop`. The boot CPU waits for every AP before continuing to
  `kinit`.

CPU ids are dense, 0 is the boot CPU. `MAX_CPUS` is 16. `kstack_top` is at
offset 24 of `struct cpu` and `user_rsp` at offset 48, for `syscall.S`.

Loading a segment register clears the GS base MSR, so the base is written
again after the GDT switch. The GDT, TSS and double fault stack of each
CPU are in `struct cpu_tables` in `gdt.c`, indexed by CPU id.
`tss_set_rsp0` writes the TSS of the calling CPU.

## Timers

`lapic_timer_calibrate` measures the APIC timer against the PIT once on
the boot CPU. `lapic_timer_start` programs the calling CPU's timer at
`TIMER_HZ` from that measurement. Every CPU takes `IRQ_TIMER`; only the
boot CPU updates the system-wide tick count. Every CPU runs its local
`sched_tick_cpu`, which drains its remote-wake inbox, accounts the running
thread's slice, expires that CPU's sleepers, performs its local priority
boost and requests a reschedule when work is ready. Each CPU counts its own
interrupts in `cpu.ticks`.

## Scheduler

`sched/mlfq.c` retains one set of MLFQ run queues, a sorted sleeper list and an
MPSC remote-wake inbox per CPU. Each run queue has its own lock, locked across
the local context switch. A remote waker publishes to the inbox and sends
`IRQ_RESCHED`; it does not take the destination queue's lock.

- A new thread is placed on the least-loaded started CPU. A blocked thread
  normally retains its home CPU so the local consumer can remove it from that
  CPU's sleeper list before making it ready.
- `sched_pick_next` takes the best thread from the calling CPU's queues.
  When they are empty it steals the highest priority ready thread of any
  other CPU, and falls back to the CPU's idle thread.
- Each CPU performs its own one-second priority boost under its local lock.
- `need_resched` and `zombie_pending` are per CPU. A thread that exits is
  finished by the next thread on the same CPU while that CPU retains its
  local queue lock across the switch.
- `waitq_wait` links the waiter before publishing `THREAD_BLOCKED`. A remote
  waker removes the waiter and queues its intrusive wake node; the target
  consumes that node after the old context has completed its switch.
- An idle CPU sits in `hlt`. `IRQ_RESCHED` wakes it immediately when a remote
  CPU publishes runnable work.

Threads running in user mode on another CPU learn about signals and
process exit at their next kernel entry, at the latest at their next
timer tick.

## TLB shootdown

`mm/tlb.c` implements `tlb_flush_range` and `tlb_drop_vmspace`. Every
unmap and protection change still calls `tlb_flush_range` with the space
lock acquired. The local TLB is flushed directly through `paging_flush_range`.
On aarch64 that flush uses the inner shareable TLBI instructions, which
reach every CPU (`PAGING_TLB_BROADCAST`), and no interrupt is sent for a
range (A8). On x86_64, if the TLBs of other CPUs may contain the
translations, those CPUs receive `IRQ_TLB_SHOOTDOWN`:

- kernel ranges go to every online CPU;
- user ranges go to the CPUs in `vmspace.cpu_mask`, which
  `vmspace_activate` maintains with atomic operations, setting the bit of
  the new space before loading `CR3` and clearing the old space's bit
  afterwards.

`tlb_lock` serializes senders. The sender writes the request (kind, space,
range), publishes the target mask in `pending`, sends one IPI per target
and spins until every target has cleared its bit. Targets service the
request in the interrupt handler or, when they spin on a spinlock with
interrupts disabled, from the spin loop through `tlb_shootdown_poll`.
This prevents a CPU that has acquired a lock and sends a shootdown from
deadlocking against a CPU that spins on that lock with interrupts
disabled. The sender is never one of its own targets. Kernel threads retain
the previous user space loaded, so `vmspace_destroy` sends a
`TLB_DROP_VMSPACE` request that makes every CPU that still has the space loaded
switch to the kernel space before the tables are freed. aarch64 sends it
as well, because a loaded root may be walked speculatively. A CPU that is
switching away from the space when the request arrives acknowledges it
without switching and clears its bit in the mask after `paging_load`, so
`vmspace_destroy` waits until the mask is empty before it frees the
tables. It asserted an empty mask before 2026-10-02, which the
`lua_prompt` case on aarch64 contradicted, because `paging_load` there
spins on `asid_lock` and services shootdowns while it spins. `tlb_get_stats`
reports rounds, IPIs and acknowledgements.

`tlb_replace_entry` replaces a present entry with one that maps another
frame or has another size. It clears the entry, flushes the range on
every CPU and only then writes the new entry. ARMv8 requires this break
before make. The copy on write of a small or huge page and the split of a
huge page into a page table use it on both architectures.

## Panic and halt

`panic` sends `IRQ_HALT` to every other CPU before printing, so only the
panicking CPU writes to the console. The halted CPUs disable interrupts
and stop. `klog` formats each line into a buffer and writes it with one
`console_write`, so log lines from different CPUs do not interleave.

## Interfaces

- `nproc()` (`SYS_nproc`) returns the CPU count, `getcpu()` (`SYS_getcpu`)
  the id of the CPU executing the caller. The `nproc` program prints the
  count.
- `smp_cpu_count`, `smp_online_mask`, `smp_active`, `cpu_by_id`.

## Races exposed by running the suite on four CPUs

- Swap eviction rewrote a page table entry to its swap slot, released the
  space lock and only then took `swap_io_lock` to write the frame. A fault
  on another CPU could take `swap_io_lock` first and read the slot before
  the data was on the disk. `evict_batch` now acquires `swap_io_lock` from
  before the first entry is rewritten until the write has completed.
- `inode_get` on two CPUs could read the same inode at the same time. The
  loser freed its duplicate `struct inode` but not the private data that
  `read_inode` had attached. The new optional `sb_ops.free_inode` releases
  that data; mfs implements it.
- The leak check in the `run` kernel test allows a short grace period for
  frees that another CPU completes after the process has been reaped.
- `sigreturn` returned through the `syscall` exit, and `sysret` loads RIP
  from RCX and RFLAGS from R11, so those two registers of the interrupted
  code were lost. On one CPU a signal almost always found the process
  blocked in a system call, where RCX and R11 are clobbered by the ABI
  anyway. With a second CPU the timer tick delivers signals to a process
  running user code, and `wsrv` resumed its fill loop with RCX pointing
  at the loop instruction. `syscall_dispatch` now enters a frame rewritten
  by `sigreturn` through `user_enter`, the `iretq` path.

## Not done

- Per CPU page caches in the buddy allocator and per CPU slab magazines
  (listed as optional).
- Interrupt routing remains on the boot CPU; device interrupts are not
  distributed.
- The Limine protocol replaces a kernel side INIT/SIPI sequence and low
  memory trampoline. The plan named that sequence as the mechanism; the
  bootloader already performs it, provides the CPU list without ACPI
  parsing, and leaves the APs in long mode.

## Tests

- `smp` (kernel): checks the CPU count against `cpus=` on the command
  line, runs eight kernel threads for 300 ms, requires that every CPU ran a
  worker and that several workers ran at the same moment. A reader thread
  on another CPU then caches the translation of a kernel page, the test
  remaps the page to another frame, and the reader must read the new
  frame. On x86_64 the remap must also have sent a shootdown round that
  reached every other CPU and was acknowledged by all of them.
- `smp_user`: `smptest` forks twice as many children as CPUs, each
  spinning 400 ms while sampling `getcpu`; the children must cover at
  least two CPUs and the whole run must take less than the sequential
  time.
- Every other case runs with four CPUs as well.
