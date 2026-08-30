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

## Logging

`klog(level, fmt, ...)` prints `[<L> <subsys>] message`. The subsystem prefix
comes from `KLOG_SUBSYS`, which a file defines before its includes (default
`kernel`). `CONFIG_LOG_LEVEL` removes lower levels at compile time, and
`loglevel=<0..3>` on the command line sets the runtime threshold.

## Assertions

`kassert(expr)` panics with the expression, file, line and function.

## Per CPU state

`struct cpu` (`arch/cpu.h`) holds the CPU id, local APIC id, current thread,
kernel stack top and the interrupt disable nesting state. The boot CPU's
instance is static in `arch/x86_64/cpu.c`. `cpu_init_boot` stores its address
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
