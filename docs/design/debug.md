# Debugging infrastructure (M2)

## Symbol table

`tools/gensyms` converts `nm -n -S` output into a blob:

    header  { u32 magic "KMYS"; u32 count; u32 strtab_off; u32 strtab_len }
    entries { u64 addr; u32 size; u32 name_off } [count], sorted by address
    strtab  NUL terminated names

Only text symbols are kept. `debug/symbols.c` reads the blob between
`__ksyms_start` and `__ksyms_end` and answers `ksyms_lookup(addr)` with a
binary search. A symbol without a recorded size extends to the next symbol.

## Backtrace

`debug/backtrace.c` walks the frame pointer chain. A frame is accepted if it
is 8 byte aligned, in the higher half and above the previous frame. The walk
stops at a zero return address, which `_start` guarantees by clearing `rbp`.
`backtrace_print_from(rip, rbp)` starts from a trap frame so exceptions show
the faulting function first. Sibling call optimization is disabled kernel wide
so that every active function has a frame.

`/dev/threads` lists every thread with its process, state, CPU, the wait
queue it blocks on (the name of the queue's lock) and its user program
counter at the entry into the kernel. For a thread that is switched out it
adds the kernel frames from the frame that `context_switch` saved
(`arch_thread_switch_frame`, `unwind_kernel`), with symbols. The frames are
read without stopping the thread, so a thread that starts to run meanwhile
can show stale frames. `cat /dev/threads` from a shell, or from a
background job while a program is blocked, shows where each thread waits.
A thread in a bounded wait also shows how long the wait has lasted.

`/dev/cpustat` lists one line `CPU USER SYSTEM IDLE` per processor with
its timer ticks since the scheduler started. `proc_account_tick` adds
each tick of a CPU to exactly one of the three counters in `struct cpu`.
A tick in the idle thread is idle time, and a tick of another thread is
user or system time by the mode of the interrupted frame. The CPU writes
its own counters with relaxed atomic stores, and the reader of the file
uses relaxed atomic loads. The counters therefore need no lock. sysmon computes
the CPU graphs from this file.

The hung task detector (`debug/hung.c`) reports waits that end soon in a
working system. `mutex_lock`, the request waits of virtio-blk and the
journal waits of mfs call `waitq_wait_bounded`, which records the start of
the wait in `thread.bounded_since`. Waits for input, for a child or in
`poll` use `waitq_wait` and are never reported. The kernel thread `hungd`
checks every thread once a second. For each bounded wait longer than the
limit it prints `hung: pid P tid T (name) in a bounded wait for N s` once,
followed by the table of `/dev/threads`, so the report also shows the
thread that holds the lock or the device that does not answer. The limit
is 30 seconds, and `hung_task=SECONDS` on the kernel command line changes
it, 0 disables the reports. Alt+SysRq prints the same table on the console
within a second. The input core detects the combination before it
delivers the key, so it also works while X12 has grabbed the keyboard.
The case `hung_task` holds a mutex for four seconds with `hung_task=2`,
checks the report and the frames down to `mutex_lock`, and feeds
Alt+SysRq through the PS/2 decoder.

## Logging

`klog(level, fmt, ...)` prints `[<L> <subsys>] message`. The subsystem prefix
comes from `KLOG_SUBSYS`, which a file defines before its includes (default
`kernel`). `CONFIG_LOG_LEVEL` removes lower levels at compile time, and
`loglevel=<0..3>` on the command line sets the runtime threshold.

## Assertions

`kassert(expr)` panics with the expression, file, line and function.

## Per CPU state

`struct cpu` (`include/cpu.h`) contains the CPU id, the current thread, the
kernel stack top, the interrupt disable nesting state and the x86_64 part
`struct arch_cpu` with the local APIC id. The instances are in the static
array of `sched/cpu.c`. `cpu_init_boot` stores its address
in `IA32_GS_BASE` and `IA32_KERNEL_GS_BASE`, and `cpu_current()` reads the
self pointer at `%gs:0`. No global variable refers to the current thread.

`push_cli` disables interrupts and, on the outermost call, records whether
they were enabled. `pop_cli` re-enables them when the depth returns to zero.

## Spinlocks

`struct spinlock` uses `xchg` for acquire and release. `spin_lock` calls
`push_cli` first, `spin_unlock` calls `pop_cli` last. With
`CONFIG_LOCKDEBUG=1` the lock records the owning CPU and the acquiring
caller, and panics on double acquisition or release by a non owner.
`spin_lock_irqsave` and `spin_unlock_irqrestore` save and restore `RFLAGS.IF`
directly. They are used for the console, which runs before `struct cpu`
exists, and for data touched by interrupt handlers that must restore the
interrupted state exactly. Lock ordering is recorded in `locking.md`.

## GDB

`.gdbinit` loads `build/kernel.elf`, connects to `:1234` and defines
`ksyms-info` and `pmm-stats`. `make gdb` prints the command line, which uses
`-iex 'set auto-load safe-path <repo>'` so the file is sourced once by
auto-load. Passing it with `-x` as well would connect twice and the QEMU stub
accepts only one client.

## Test

`tests/cases/backtrace` boots with `test=backtrace`. `test_bt_a` calls
`test_bt_b` calls `test_bt_c`, which reads a canonical unmapped address. The
expected serial output contains the `#PF` line and the symbolized names
`test_bt_c`, `test_bt_b`, `test_bt_a`, `test_backtrace`,
`ktest_run_selected` and `kmain`.

## Kernel self tests

`tests/ktest.h` provides `KTEST_DEFINE(name, fn)`, which places a descriptor
in the `.ktests` section. `ktest_run_selected` looks up the `test=` command
line value and runs the match. Returning from the test prints `TEST PASS`,
`ktest_fail` prints `TEST FAIL <reason>`. Both exit QEMU through
isa-debug-exit.
