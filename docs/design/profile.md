# Sampling profiler (M41)

## Sampling

`kernel/debug/profile.c` hooks the timer interrupt of every CPU
(`profile_sample`, called after the CPU time accounting). While a session
is enabled, and the interrupted thread is not the idle thread and belongs
to the profiled process (or any process when the filter is 0), and the
CPU's tick count is a multiple of the divider, the handler builds a
`struct prof_sample`: pid, tid, CPU, a user/kernel flag, the interrupted
instruction pointer and up to seven return addresses walked through frame
pointers (both the kernel and user programs are built with
`-fno-omit-frame-pointer`). In user mode every load of the walk goes
through `vmm_translate`, so a corrupt frame pointer ends the chain instead
of faulting in the interrupt handler; in kernel mode the chain must stay
inside the current kernel stack. Each CPU publishes into its own 8192-entry
SPSC ring, so the timer path takes no global profiler lock. A full local ring
counts a drop instead of overwriting, so its oldest samples survive. The
profile device's object-local poll source wakes readers when a ring changes
from empty to nonempty.

## /dev/profile and /dev/ksyms

`/dev/profile` controls the session: `PROF_START` (argument: pid, 0 for
all) allocates one ring for every started CPU on first use, clears the
counters and enables sampling; `PROF_STOP` disables it;
`PROF_SET_DIVIDER` sets the ticks
between samples on each CPU (1 to 1000, so 1 kHz down to 1 Hz per CPU);
`PROF_GET_STATS` fills `struct prof_stats` with the sample and drop counts,
the pending count, the state and the ring capacity. `read` returns whole
samples from the ring, `poll` reports `POLLIN` while samples are pending,
and closing the device ends the session. Close first disables sampling,
waits for every per-CPU active counter to reach zero, then frees the rings,
so an in-flight timer interrupt cannot publish into released memory and a
program that exits leaves nothing behind. One session exists at a time.

`/dev/ksyms` prints the kernel symbol table as `addr size name` lines,
regenerated from the `.ksyms` blob at each read, so user space can name
kernel addresses.

## Client library

`minios/profile.h` in libc wraps the device (`prof_open`, `prof_start`,
`prof_stop`, `prof_set_divider`, `prof_get_stats`, `prof_read`), loads
symbol tables (`prof_symtab_load_elf` reads the `.symtab` function symbols
of a static ELF such as `/bin/<name>`, `prof_symtab_load_kernel` parses
`/dev/ksyms`), looks addresses up (`prof_symtab_lookup` attributes an
address to the preceding symbol up to the start of the next one, so return
addresses after `noreturn` calls resolve) and keeps a histogram of string
keys (`prof_hist_add`, `prof_hist_sort`).

## Lock attribution (M42)

A kernel sample whose innermost frames are the lock primitives
(`pop_cli`, `push_cli`, `spin_lock`, `spin_unlock` and the `irqsave`
variants) is attributed by `prof_attribute` to the first frame outside
them and marked `(locked)`: the timer interrupt was pending while
interrupts were disabled and fired on the `sti` of the release. `prof -a`
samples every process, prints a table of samples per process and
symbolizes each sample with the binary of its own process. See
`lockstat.md`.

## Tools

`prof command args...` starts the command behind a pipe so sampling is on
before it runs, filters on its pid, drains the ring while waiting for it
and prints the flat profile (percent, samples, symbol, `[kernel]` marker);
`-k` includes kernel samples, `-c` also aggregates whole call chains,
`-n` limits the rows, `-p pid -d seconds` samples a running process.
`sysmon`'s Profile tab does the same for the selected process (or all)
with a live table (`tools.md`).

## Test

`tests/cases/profile` runs `/bin/proftest`: symbol table loading and
lookups (own binary and kernel), a hot function taking most of the user
samples of the process with chains reaching `main`, the pid filter with a
spinning parent excluded, kernel samples of a child making system calls
resolving through `/dev/ksyms`, the divider, the absence of samples after
stop, ring overflow accounting when four spinning processes are sampled
without reading, and `poll`.
