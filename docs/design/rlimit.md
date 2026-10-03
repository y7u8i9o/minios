# Resource limits and CPU accounting (M40)

## Limits

Every process carries `struct rlimit rlim[RLIMIT_NLIMITS]` (`sched/proc.h`).
The kernel process contains the defaults (everything unlimited except
`RLIMIT_NOFILE`, which is `OPEN_MAX`, and the 8 MiB soft `RLIMIT_STACK`);
`proc_setup` copies the parent's table into a new process, and exec retains
it. `getrlimit`, `setrlimit` and `prlimit` (`syscall/sys_rlimit.c`) read and
write the table under `proc.lock`; the enforcement points read the soft
limit without the lock through `proc_rlimit_cur`, since a stale value only
moves a check by one call. The refusals are a soft limit above the hard one
(`EINVAL`) and a `RLIMIT_NOFILE` hard limit above `OPEN_MAX` (`EPERM`), and
since U2 of the multiuser plan (`users.md`) the raising of a hard limit by
anyone but root (`EPERM`) and `prlimit` on a process of another user
(`EPERM`).

| Limit | Enforced by |
|---|---|
| `RLIMIT_AS` | `sys_mmap` and `sys_sbrk` compare `vma_total_size` plus the request, `ENOMEM` |
| `RLIMIT_DATA` | `sys_sbrk` compares the heap size (break minus heap start) after growth, `ENOMEM` |
| `RLIMIT_STACK` | `proc_exec` and `proc_create_user` size the main stack region by the creating process's soft limit, clamped to 64 KiB to 1 GiB; the arguments may use half of it |
| `RLIMIT_NOFILE` | `fdtable.limit`: `fdtable_install` searches below it (`EMFILE`), `fdtable_install_at` refuses slots above it (`EBADF`); open descriptors above a lowered limit remain usable |
| `RLIMIT_NPROC` | `sys_fork` counts live user processes, `EAGAIN` |
| `RLIMIT_FSIZE` | `file_write` on a regular file clamps the count to the limit and, at the limit, sends `SIGXFSZ` and returns `EFBIG` |
| `RLIMIT_CPU` | the timer tick sends `SIGXCPU` when the process's user plus system ticks reach the soft limit and once per second afterwards, `SIGKILL` at the hard limit |
| `RLIMIT_CORE`, `RLIMIT_RSS`, `RLIMIT_MEMLOCK` | stored only |

`SIGXCPU` and `SIGXFSZ` are ordinary signals whose default action
terminates the process.

## Accounting

`proc_account_tick` runs from the timer interrupt of every CPU. It charges
the interrupted thread and its process with one user or system tick
according to the privilege level in the trap frame (`thread.utime`,
`thread.stime`, `proc.utime`, `proc.stime`; the process fields are updated
atomically because several CPUs may run threads of one process), then
checks `RLIMIT_CPU`. The idle thread is not charged.

`sched_switch_locked` counts a switch away from a thread that is still
ready as involuntary and every other switch as voluntary
(`nvcsw`/`nivcsw` on thread and process). `vma_resolve_fault` counts
faults: swap ins and file page reads are major, everything else minor.
Since M46, every address space maintains a per-CPU resident-page counter at
each present-PTE installation and removal point. `vma_count_resident` sums
the slots for `/dev/proc` and `getrusage`; a huge mapping adds or removes 512
pages at once, so neither interface walks page tables.

`proc_reap` adds the child's totals (and the child's own children totals)
to the reaper's `c*` fields under the reaper's lock. `getrusage` reports
`RUSAGE_SELF`, `RUSAGE_CHILDREN` and `RUSAGE_THREAD`; `wait4` fills its
`rusage` argument from the child before reaping it. Ticks become
`struct abi_timeval` at `TIMER_HZ`, so the resolution is one millisecond.

`/dev/proc` gained `TIME` (ticks of user plus system time) and `RSS` (KiB)
columns before `NAME`; `ps` prints the table as is and `sysmon` parses the
new columns.

## User space

`sys/resource.h` declares the four calls and takes the structures and
constants from `minios/abi.h`. The shell's `ulimit` builtin (`-HS`,
`-acdfnstuv`, sizes in KiB, `-f` in 512 byte blocks) changes the shell's
own limits, which its children inherit. `prlimit [-p pid] [RES[=soft[:hard]]]`
lists or changes the limits of any process, and `time command` reports
real, user and system time plus the resident size, fault and switch counts
from `wait4`.

## Test

`tests/cases/rlimit` runs `/bin/rlimittest`: defaults, validation errors,
inheritance and `prlimit` on a child, `RLIMIT_AS` and `RLIMIT_DATA`
refusing `mmap` and `sbrk`, `RLIMIT_NOFILE` against `open`, `dup` and
`dup2`, `RLIMIT_NPROC` against `fork`, `RLIMIT_FSIZE` clamping a write and
delivering `SIGXFSZ` (handled and default), `RLIMIT_STACK` measured by an
exec'd child probing below its stack, `RLIMIT_CPU` terminating a spinning
child with `SIGXCPU` and then `SIGKILL`, and `getrusage`/`wait4` times,
faults, resident size and switches.

The `dup2` failure above the soft descriptor limit must drop the temporary
reference acquired by `fdtable_get`. Otherwise the file and its inode remain
allocated after process teardown even though every user-visible limit check
passes. The test runner's unchanged physical-page comparison catches this.
