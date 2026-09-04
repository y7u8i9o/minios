# Signals and process control

## Signal state

Every process carries a pending mask and a table of dispositions
(`struct ksigaction`: handler, mask, flags, restorer) protected by
`proc.lock`; every thread carries its own blocked mask, touched only by
the thread itself. Signal numbers and `struct sigaction` are defined in
`kernel/include/minios/abi.h` and shared with libc.

`signal_send(p, sig)` is the single entry point. `SIGKILL` terminates
the process immediately through `proc_begin_exit`. Otherwise the
disposition decides: a signal that is ignored, or whose default action is
to ignore (`SIGCHLD`, `SIGWINCH`, and `SIGCONT` after it performs the
continue action), is dropped; any other signal sets its pending bit
and wakes threads blocked in the kernel that do not block it, using
`waitq_interrupt`. Process 1 is never terminated by a default action: a
signal without a handler is dropped and `SIGKILL` is refused, so a stray
`kill` cannot bring the system down.

Blocking operations (`wait4`, console reads, pipe reads and writes,
`thread_join`) call `signal_should_interrupt` when they wake and return
`EINTR` if the process is exiting or an unmasked signal is pending. The
signal is then delivered on the way back to user mode.

`fork` copies the dispositions and the thread mask and clears the pending
set. `exec` resets caught signals to the default and keeps ignored ones.

## Delivery

`signal_deliver` runs on every return to user mode, after the exit check
in `syscall_dispatch` and `trap_dispatch`. It takes the lowest pending
unblocked signal and clears it. The default action for every signal except
the ignored signals and the job-control stop signals terminates the process
with status `PROC_STATUS_SIGNALED(sig)`, which `wait4` reports. `SIGSTOP`,
`SIGTSTP`, `SIGTTIN`, and `SIGTTOU` stop every thread in the process;
`SIGCONT` returns stopped threads to the run queues. `SIGSTOP` cannot be
caught, ignored, or blocked. `wait4` reports these transitions with
`WUNTRACED` and `WCONTINUED` without reaping the process.

For a handler the kernel pushes a `struct sigframe` on the user stack,
below the 128 byte red zone and 16 byte aligned as after a call: the
restorer address in the return slot, the complete trap frame, the saved
blocked mask and the signal number. The trap frame is rewritten to enter
the handler with the signal number in `rdi`. The handler's mask and the
signal itself (unless `SA_NODEFER`) are added to the thread mask while it
runs. The handler's `ret` lands in libc's `__sigreturn_trampoline`, which
issues `sigreturn`. `signal_return` copies the saved frame back, forcing
the user code and stack selectors and sanitizing `RFLAGS`, and restores
the mask, so the interrupted code resumes exactly where it was, including
the `EINTR` result of an interrupted system call.

Synchronous faults use `signal_fault`: a user page fault that the memory
manager cannot resolve queues `SIGSEGV` when a handler is installed and
delivers it before returning, otherwise the process is killed as before.
`SIGPIPE` is sent to a writer on a pipe without readers before `EPIPE` is
returned.

## System calls

`sigaction`, `sigprocmask`, `sigreturn`, `kill` (positive pid, 0 for the
caller's group, negative for a group, -1 for every process except init,
signal 0 to probe), `setpgid`, `getpgid`, `tcsetpgrp`, `tcgetpgrp` (the
console is the only terminal, the descriptor is ignored) and `reboot`.
libc adds `signal`, `raise`, the `sigset_t` helpers and `strsignal`.

## Process groups and terminal job control

Each process has a group id under `proc_tree_lock`, initially its own
pid, inherited across `fork`. The keyboard line discipline treats control
C as an interrupt and control Z as a stop request: it echoes the control
key, discards the partial line and wakes the `ttyd` kernel thread, which
sends `SIGINT` or `SIGTSTP` to the foreground group
recorded by `tcsetpgrp`. Posting a signal takes process locks, which is
why it happens in a thread rather than in the interrupt handler.

The shell ignores `SIGINT`, `SIGPIPE`, `SIGTSTP`, `SIGTTIN`, and `SIGTTOU`
itself, puts every pipeline into the group of its first process, makes that
group the foreground group while the pipeline runs, restores the defaults
in each child before `exec`, and takes the console back afterwards. A
process group that reads the console while it is not foreground receives
`SIGTTIN`. The `fg` and `bg` builtins continue stopped groups with
`SIGCONT`; only `fg` transfers the console. `init` puts the shell into its
own group.

## Process listing

`/dev/proc` renders the process table (`PID PPID PGID STATE TIME RSS NAME` since M40) on
every read; the `ps` utility prints it. `kill [-SIG] pid...` sends
signals by number or name.

## Orderly shutdown

`reboot(cmd)` accepts `RB_POWER_OFF`, `RB_AUTOBOOT` and `RB_HALT` and may
only be called by process 1. It sends `SIGTERM` to every other process,
waits up to two seconds for them to exit, sends `SIGKILL` to survivors
and waits again, flushes and unmounts every filesystem that is not busy
(`vfs_umount_all`), then powers off through ACPI, reboots through the
keyboard controller or halts with interrupts disabled. Programs never
call it directly: `shutdown` sends `SIGUSR1` to init and `shutdown -r`
or `reboot` sends `SIGUSR2`; init's handler records the request and the
main loop, woken from `wait` by `EINTR`, calls `reboot`. `power_off`
waits after the ACPI write so the ACPI exit is not overtaken by the
`isa-debug-exit` fallback.

## Tests

- `signals` runs `/bin/sigtest`: handler invocation and return through
  `sigreturn` with registers intact, blocking and deferred delivery,
  default termination, ignoring `SIGTERM` and the impossibility of
  ignoring `SIGKILL`, `SIGCHLD` on child exit, `EINTR` on a pipe read,
  a `SIGSEGV` handler, `SIGPIPE`, disposition reset by `exec`, process
  groups with `kill(-pgid)`, protection of init and the error cases.
- `ctrlc` starts the shell, waits until `cat` is reading the console,
  feeds control C through the keyboard driver and expects
  `[cat terminated by signal 2]`, then checks `ps` output and `kill` on a
  missing pid.
- `jobcontrol` exercises control Z, stopped/continued wait statuses,
  `jobs`, `bg`, `fg`, background state, and final control C delivery.
- `shutdown_cmd` types `shutdown` into a session run by the real init and
  verifies on the host that QEMU exited through ACPI with a clean image
  containing the file written before.
- `kbd` covers the new control C line discipline behaviour.

## Return path (M18)

`sigreturn` restores the interrupted register state into the trap frame
of the system call. Because `sysret` takes RIP from RCX and RFLAGS from R11,
`syscall_dispatch` enters that frame through `user_enter`, which returns
with `iretq` and restores every register. Signal delivery on the way out
of an interrupt already uses `iretq`.
