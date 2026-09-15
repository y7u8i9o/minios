# Init

`user/init/init.c` is process 1. It reads `/etc/init.conf`, runs the boot
tasks in order, supervises the services and the console session,
answers `initctl` on a control socket, reaps orphaned processes and
performs the orderly shutdown. It is linked statically, as before, so
that it depends on nothing under `/lib`.

## Configuration

`/etc/init.conf` (`init.conf(5)`) has one entry per line: `env` lines
extend the environment of every program started afterwards, `task`
entries run to completion in file order, `service` entries run in the
background and are restarted, and the `console` entry is the session on
the console, started in its own process group with the terminal
(`setpgid`, `tcsetpgrp`). Options between the name and the command are
`if=PATH` (start only when the path exists), `log=FILE` (standard
output and error appended to the file) and `restart=always|never|failure`.
The shipped file runs `fsinit`, then `net apply`, then the shell. When
the file cannot be read init parses a built-in table with the same
entries, so a damaged root image still boots to a shell.

Entries are records in a fixed array (`struct entry`, at most 32). The
command words are kept NUL separated in the record and the argument
vector is rebuilt at every start, so records can be copied and moved
when the configuration is reloaded.

## Supervision

`start_entry` forks, closes the control descriptors in the child, sets
up the process group and the log file, and executes the command. A task
is waited for at once (`wait_for`, which reaps other children meanwhile).
The main loop reaps every exited child with `waitpid(-1, WNOHANG)`
(children of programs that exited are reparented to init and reaped the
same way), applies the exit to the entry it belonged to
(`entry_exited`), starts what is due, and sleeps in `poll` on the
control socket. `SIGCHLD` has a handler only so that it interrupts the
poll; the one second poll ceiling covers a signal that arrives between
the reap and the poll.

A service that exits is restarted immediately when it ran for at least
five seconds, otherwise after one second. Five such quick exits in a row
mark it failed; `initctl start` clears the count. The console entry is
always restarted, since the system is unusable without a session. Stop
requests send `SIGTERM` (to the process group for the console), wait up
to three seconds and then send `SIGKILL`; the request is answered when
the process has been reaped, so `initctl status` right after a stop
shows the final state.

## Control socket

Init listens on the abstract Unix socket `init` (`sockets.md`). A
request is one text line, the reply starts with `ok` or `error: message`
and continues with the output. `initctl` (`user/coreutils/initctl.c`)
joins its arguments into the request, prints the body and exits with the
first line's verdict. The commands are `list`, `status`, `start`, `stop`,
`restart`, `reload [FILE]`, `poweroff`, `reboot` and `halt`. A reload
reads the file again: entries no longer present are stopped and removed,
new ones start, and an existing entry keeps its state with the new
command taking effect at its next start.

## Shutdown

`SIGUSR1`, `SIGUSR2` and `SIGHUP` (sent by `shutdown`, `reboot` and
`halt`) and the corresponding requests set a flag that the main loop
turns into `shutdown_system`: every running entry gets `SIGTERM` in
reverse configuration order, init waits up to three seconds for them,
prints the final line and calls `reboot(2)`, which terminates whatever
is left, unmounts and stops the machine.

Two kernel changes came with the new init. The console output of the
kernel is queued per CPU and drained by a thread; the old init reached
`reboot(2)` only after the kernel had waited for the shell, which gave
the drain thread time, while the new one has already stopped everything
and the final lines were lost. `sys_reboot` now calls `console_flush`
before halting. Second, the regions of a reaped process release their
file references through RCU callbacks, and a dynamically linked init
holds the shared libraries mapped; both kept `/lib/libc.so` referenced
and the root filesystem busy. `sys_reboot` removes the regions of init
(it never returns to user mode) and waits with the new `rcu_synchronize`
before `vfs_umount_all`. The `shutdown` case, whose init is the dynamic
`shutdowntest`, had been failing on this since dynamic linking.

## Tests

`tests/cases/initctl` (`test_initctl` in `kernel/tests/test_shell.c`)
boots the real init with a typed script: a service added through
`initctl reload` of a copied configuration is stopped, started and
restarted, a command that keeps failing is given up after five quick
exits with seven starts on record, a reload of the original file removes
it, unknown commands and names are reported, and `initctl poweroff` goes
through the orderly shutdown. `shutdown_cmd` and `shutdown` cover the
signal path and the kernel side.
