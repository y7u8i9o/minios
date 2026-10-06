# Sockets, descriptor passing and descriptor flags

M23 adds the kernel primitives the display server rework relies on.

## Unix domain stream sockets (`kernel/ipc/socket.c`)

- `socket(AF_UNIX, SOCK_STREAM, flags)` creates an unbound socket;
  `bind` attaches an abstract name (`struct sockaddr_un.sun_path`, at
  most `SOCK_NAME_MAX` bytes, no filesystem entry); `listen` registers
  the name in a table of at most 16 listeners; `connect` looks the name
  up and queues a new connection in the listener's backlog (8 entries,
  `ECONNREFUSED` beyond that or for an unknown name); `accept` dequeues
  a connection and returns the server end; `socketpair` creates a
  connected pair directly; `shutdown` closes one or both directions.
- A connection contains two directions, each a 64 KiB ring (16 pages from
  the buddy allocator) with a read and a write wait queue. Reads and
  writes block, or return `EAGAIN` when the descriptor is non blocking;
  data moves through a 256 byte bounce buffer so no lock is locked while
  user memory is touched (as in `pipe.c`). When the peer has closed,
  reads return end of file and writes return `EPIPE` after raising
  `SIGPIPE`. `poll` reports `POLLIN` (data or peer closed), `POLLOUT`
  (room or peer closed), `POLLHUP` (both directions ended), and
  `POLLIN` on a listener with pending connections.
- Descriptor passing: `sendmsg` with an `SCM_RIGHTS` control message
  (at most 16 descriptors) attaches referenced files to the stream
  position of the message's first byte; `recvmsg` receives them with
  the read that consumes that byte, installs new descriptors in the
  receiver and fills its control buffer, and never reads past the
  start of a later message that carries descriptors. Without room in
  the control buffer the files are released and `MSG_CTRUNC` is set.
  Files still queued when a connection is freed are released.
- Locks: `sock_table_lock` (listener names) is taken before
  `sock->lock` (backlog); `conn->lock` (rings and records) is never
  nested with them.
- Since N01 (`docs/design/network.md`) the Unix code is the backend of a
  common socket layer (`kernel/ipc/socket.c`): the file, the poll source,
  address copies, flag validation and the `SOL_SOCKET` options live
  there, `send` and `recv` carry flags, and `getsockname`,
  `getpeername`, `setsockopt` and `getsockopt` exist.
- Since U4 of the multiuser plan (`users.md`) `getsockopt` with
  `SO_PEERCRED` returns the `struct ucred` (pid, effective uid and gid) of
  the other end: the connecting process for the accepted socket, the
  process that called `listen` for the connecting one, and the creator for
  both ends of a socket pair. `struct conn` records both under
  `conn.lock`, and `struct unix_sock` retains the listener's under its
  `lock`.
- `unix_socket_accepting(name)` reports whether a thread waits for
  connections on the listener bound to `name`. The thread waits in
  `accept` on the listener's `accept_waitq`, or in a `poll` call that has
  an entry on the listener's poll source. The GUI boot tests use the
  result as the readiness signal of X12 (`gui.md`).

## Anonymous shared memory (`memfd_create`, `ftruncate`)

`memfd_create` returns a descriptor on an unnamed shm object
(`kernel/ipc/shm.c`, `shm_create_anon`) that starts empty; `ftruncate`
grows it page by page (shrinking is refused). The descriptor is mapped
with `mmap(MAP_SHARED)` and can be passed over sockets; the object is
freed with its last descriptor and mapping. `struct file_ops` gained
`truncate` for this.

Since G8 of `docs/plan/compositor-performance.md`, a page of a shm
object gets its frame at the first fault in any process. `ftruncate` and
`shm_open` allocate only the page array. `mmap` adds a `VM_SHM` region
that references the file. A fault in such a region calls the `page`
operation of `struct file_ops`. `shm_page` returns the frame of the
page and allocates a zero filled frame at the first use. Fork copies the
region with its file reference, so a page that the child touches first
is shared with the parent. `mqtest` checks this case.

## Event and timer descriptors (`kernel/ipc/eventfd.c`, `timerfd.c`)

`eventfd` contains a 64 bit counter: `write` adds, `read` returns and
clears it, blocking or `EAGAIN` while zero. `timerfd_create` with
`timerfd_settime(fd, {initial_ms, interval_ms})` counts expirations
from the timer interrupt (`timerfd_tick` runs on the boot CPU every
tick); `read` returns the count and clears it, `poll` reports
`POLLIN`, `timerfd_gettime` reports the time to the next expiration.

## Descriptor flags

`struct file.flags` carries `O_NONBLOCK`, honoured by sockets, pipes,
message queues, eventfd and timerfd. `struct fdtable` stores a close on
exec bit per descriptor: set by `O_CLOEXEC` on `open`, `pipe2`,
`socket`, `socketpair`, `accept4`, `eventfd`, `timerfd_create` and
`MFD_CLOEXEC`, by `fcntl(F_SETFD)` and `F_DUPFD_CLOEXEC`, copied by
`fork`, cleared when a slot is reused, and applied by `execve`
(`fdtable_close_exec`). `fcntl` also supports `F_GETFD`, `F_GETFL`,
`F_SETFL` (`O_NONBLOCK`, `O_APPEND`) and `F_DUPFD`.

## poll

`poll` accepts up to 64 descriptors and defines `POLLERR`, `POLLHUP`
and `POLLNVAL`. A positive timeout now sleeps on the poll wait queue
with a deadline (`waitq_wait_timeout`, `kernel/sched/wait.c`): timed
waiters are retained on a list scanned by the timer interrupt, which wakes
expired ones through `waitq_interrupt`; the previous implementation
polled every 5 ms.

## Tests

`sockets` (`/bin/socktest`), `evfd` (`/bin/evfdtest`), `fdflags`
(`/bin/fdflags`, which re-executes itself to check close on exec) and
`fpu` (`/bin/fputest`, see `scheduler.md`).
