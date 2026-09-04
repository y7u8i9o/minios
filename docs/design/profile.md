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
inside the current kernel stack. The sample goes into a ring of 8192
entries under `prof_lock`; a full ring counts a drop instead of
overwriting, so the oldest samples survive. `poll_notify` wakes readers
when the ring goes from empty to non empty.

## /dev/profile and /dev/ksyms

`/dev/profile` controls the session: `PROF_START` (argument: pid, 0 for
all) allocates the ring on first use, clears the counters and enables
sampling; `PROF_STOP` disables it; `PROF_SET_DIVIDER` sets the ticks
between samples on each CPU (1 to 1000, so 1 kHz down to 1 Hz per CPU);
`PROF_GET_STATS` fills `struct prof_stats` with the sample and drop counts,
the pending count, the state and the ring capacity. `read` returns whole
samples from the ring, `poll` reports `POLLIN` while samples are pending,
and closing the device ends the session and frees the ring, so a program
that exits leaves nothing behind. One session exists at a time.

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
