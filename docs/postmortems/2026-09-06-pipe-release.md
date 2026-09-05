# Postmortem: pipe release race in the utils boot test

| Field | Value |
| --- | --- |
| Document | PM-2026-09-06-01 |
| Version | 1.0 |
| Date | 2026-09-06 |
| Branch | `bleeding-edge-terminal` at commit `3b8431d` |
| Status of the fix | Applied in the working tree, not committed |

## 1 Purpose

This document records a failure of the `utils` boot test. It gives the cause,
the correction, the verification, and the lessons.

## 2 Audience

This document is for the developers of minios. You must know the kernel
process model, the pipe implementation, and the boot test runner.

## 3 Summary

The `utils` boot test failed at random. Some runs stopped for more than
90 seconds. One run reported a general protection fault and then a nested
panic. Other runs passed.

The cause was a race in `pipe_release` in `kernel/ipc/pipe.c`. When the two
ends of a pipe closed at the same time on two CPUs, one release touched the
pipe after the other release freed it. The correction adds a reference count
to the pipe. Each release drops its reference as its last step.

The new boot test `pipe_close` reproduces the race. It fails on the old
kernel and passes on the corrected kernel.

## 4 Symptoms

The test runner reported these results for the `utils` case:

- Timeout after 90 seconds, at different points of the test script.
- A general protection fault (exception 13) in kernel mode, followed by a
  nested panic. The serial log had no register dump and no backtrace.
- Complete passes in other runs.

No utility check failed in a consistent way.

## 5 Timeline

All times are on 2026-09-06.

| Time | Event |
| --- | --- |
| 02:04 | Commit `3b8431d` completes the utility work of the terminal plan. |
| 02:06 | A previous session runs the `utils` case and sees the random failures. It saves a gdb snapshot of one stopped run. |
| 02:15 | The investigation rebuilds the tree and runs 20 copies of the `utils` case. All 20 fail at the `uname -a` check. |
| 02:20 | 12 runs under TCG give one general protection fault. A short script with six command substitutions stops in 1 run of 4. |
| 02:25 | The image is rebuilt. The `uname -a` failure does not occur again. |
| 02:35 | 12 runs with gdb attached give one stopped run. The gdb snapshot shows the spinning thread. |
| 02:45 | The correction, the test, and the panic change are applied. |
| 03:05 | Verification completes. |

## 6 Investigation

### 6.1 First snapshot

The previous session saved a gdb snapshot of a stopped run
(`minios-utils-gdb.log`). It showed:

- Process 1 (`sh`) blocked in `sys_wait4`, on the wait queue `proc_child`.
- Process 211 (`sh`) in state `PROC_RUNNING` with no thread in its thread
  list.
- All four CPUs in the idle loop.

The gdb script walked only the `threads` list of each process. A thread that
has called `thread_exit` is on the `zombies` list. The snapshot therefore hid
the thread of process 211.

### 6.2 Reproduction

The host is an arm64 Mac. QEMU runs the guest with TCG only.

The investigation ran four copies of the `utils` case in parallel, in
batches. Results before the correction:

| Runs | Result |
| --- | --- |
| 20 | 20 failures at the `uname -a` check, caused by a stale disk image (see 7.2) |
| 12 | 1 general protection fault with nested panic |
| 12, gdb attached | 1 stopped run |

### 6.3 Second snapshot

The investigation attached gdb to each run through a QEMU wrapper script.
After 80 seconds, the script interrupted the guest and walked the process
list, including the `zombies` list of each process.

The stopped run showed:

- Process 1 (`sh`) blocked in `sys_wait4`.
- Process 95 (`sh`), child of process 1, with `nthreads` 0 and `exiting` 1.
- Thread 104 of process 95 on the `zombies` list, in state `THREAD_RUNNING`
  on CPU 0.
- The backtrace of CPU 0:

```
spin_lock            sync/spinlock.c:161
poll_source_notify   ipc/poll.c:30
pipe_release         ipc/pipe.c:123
file_put             fs/file.c:45
fdtable_close        fs/file.c:210
fdtable_close_all    fs/file.c:259
proc_exit_notify     sched/proc.c:141
thread_exit          sched/thread.c:126
sys_exit             syscall/sys_proc.c:17
```

The thread spun on the lock of the poll source inside a pipe. The slab
allocator fills freed memory with the byte `0x6b`. A lock word with this
value reads as held. The pipe was freed.

The serial log of this run ended after `yes` (pid 78) received `SIGPIPE`.
This is the check `yes | head -n 2` in `user/etc/tests/utils.sh`. Both
processes of that pipeline exited at the same time.

## 7 Causes

### 7.1 Root cause

The old `pipe_release` did these steps:

1. Take `pipe.lock`.
2. Decrement `readers` or `writers`.
3. Compute `gone` as `readers == 0 && writers == 0`.
4. Wake the readers and the writers.
5. Release `pipe.lock`.
6. Notify the pollers.
7. If `gone`, free the pipe.

Consider two CPUs. CPU A releases the read end and CPU B releases the write
end. CPU A completes step 5 with `gone` false. CPU B then completes all
steps and frees the pipe. CPU A then does step 6 on freed memory.

The result depends on the content of the freed memory:

- The lock word reads as held. CPU A spins forever. The parent waits forever
  for the child. The test runner reports a timeout.
- The waiter list holds invalid pointers. CPU A dereferences a non canonical
  address. The CPU raises a general protection fault.

The race is old. The terminal branch made it visible because its shell
runs many more short pipelines and command substitutions.

### 7.2 Contributing causes

The investigation was slow for three reasons.

**Stale disk image.** The target `make` rebuilds the programs but not
`build/disk.img`. Only `make test` rebuilds the image. The image from the
previous session held old binaries. It made the `uname -a` check fail in
every run. This failure had no relation to the race.

**Lost panic report.** When CPU A faulted, CPU B also faulted on the same
memory. The halt IPI cannot stop a CPU that is already inside a fault. CPU B
entered `panic`, saw `panic_in_progress`, printed `nested panic`, and exited
QEMU. CPU A had not yet printed the frame and the backtrace. The serial log
held no register dump.

**Incomplete snapshot.** The first gdb script walked only the `threads`
list of each process. It did not show exiting threads.

## 8 Corrections

All changes are in the working tree of `bleeding-edge-terminal`.

| File | Change |
| --- | --- |
| `kernel/ipc/pipe.c` | `struct pipe` gets `refs`, a reference count set to 2 at creation. `pipe_release` drops one reference as its last step and frees the pipe only when the count reaches 0. |
| `kernel/tests/test_pipe.c`, `tests/cases/pipe_close` | New boot test. Two kernel threads release the read ends and the write ends of 256 pipes in step, for 40 rounds. The threads wait for a start flag with `sched_yield`. |
| `kernel/debug/panic.c` | A second CPU that enters `panic` during a report halts. Only a nested panic on the reporting CPU prints `nested panic` and exits. |
| `kernel/sched/proc.c` | `proc_free` unlinks the process from the children list of its parent. A fork that failed before the child ran left the child in that list. `proc_reap` now resets the link after removal. |
| `kernel/sched/user.c` | Kernel started processes get `/dev/console` read/write on descriptors 0, 1 and 2. This lets `less` read keys from `dup(1)` when its input is a pipe. |
| `docs/design/vfs.md`, `docs/design/console.md` | Design documents updated. |

## 9 Verification

### 9.1 Regression test

| Kernel | `pipe_close` result |
| --- | --- |
| Old `pipe.c` | Timeout after 60 seconds |
| Corrected `pipe.c` | Pass |

### 9.2 Repeated runs on the corrected kernel

| Case | Runs | Passed |
| --- | --- | --- |
| `utils` | 20 | 20 |
| `pipe_close` | 12 | 12 |
| `pipes` | 8 | 8 |

All runs used four guests in parallel under TCG.

### 9.3 Boot cases

These cases pass on the corrected kernel: `pipe_close`, `pipes`, `fs`,
`shell`, `shell2`, `script`, `script2`, `utils`, `jobcontrol`, `ctrlc`,
`fork`, `signals`, `exception`, `slab_redzone`, `lineedit`,
`lineedit_screen`, `console_sgr`, `mint`, `libc_ext`, `games`, `gui_term`.

### 9.4 Interactive check of less

Screenshots of the console show that `ls /bin | less`:

- Shows the first page with the status line `standard input 1/143 32%`.
- Moves one page forward on `space`.
- Reports `pattern not found` for a search that has no match below the
  current page.
- Returns to the shell prompt on `q`.

## 10 Lessons

1. An object shared by two file ends must keep a reference for each end.
   Release the reference after the last access to the object.
2. Rebuild `build/disk.img` after `make` before you run cases through
   `tests/run_all.sh` directly. Use `make test CASES=...` when in doubt.
3. A gdb script that walks processes must walk the `zombies` list as well
   as the `threads` list.
4. Run a random failure many times in parallel. Attach gdb through a QEMU
   wrapper and take a snapshot when the run stops.
5. The panic path must let the first CPU complete its report.

## 11 References

- `docs/design/vfs.md`, section Pipes
- `docs/design/console.md`, section Panic and power
- `fucking_die_if_you_add_co_authered_by_again/minios-utils-gdb.log`, the
  first gdb snapshot
- `tests/cases/utils`, `tests/cases/pipe_close`

## 12 Glossary

| Term | Meaning |
| --- | --- |
| End | One of the two files of a pipe: the read end or the write end. |
| Nested panic | A call to `panic` while another panic is in progress. |
| Poller | A thread that waits in `poll` for a file to become ready. |
| TCG | The QEMU software CPU emulator. |
| Zombie list | The list of threads of a process that have exited but are not yet reaped. |
