# Networking

The TCP/IP stack is built in the `N` milestones of `NETWORK_PLAN.md`. This
document describes the contracts fixed before the protocol code exists
(N00), the common socket layer (N01) and the packet core with its worker
(N02). Later milestones extend it.

## Feature matrix

The initial target is IPv4 host operation on QEMU's q35 machine with one
modern virtio-net device and the loopback interface. Checksums are computed
in software; no segmentation or checksum offload is negotiated. IPv6,
forwarding, multicast, raw sockets for applications, other NIC drivers and
multiple queue pairs are outside the plan. The specifications consulted
are recorded here as their milestones land:

| Area | Specification | Supported subset |
|---|---|---|
| Packet checksums | RFC 1071 | one's complement sum over 16 bit words, odd trailing byte padded with zero |

## Contracts

These rules bind every networking milestone.

### Socket dispatch

A socket is one `struct socket` (`kernel/include/ipc/socket.h`) with a
family, a type, a protocol, a backend operation table, the pending
asynchronous error and one poll source. Descriptor installation, close on
exec, file references, address copies and message flag validation are done
once in the common layer; backends see kernel copies only. `SCM_RIGHTS`
processing belongs to the Unix backend. A backend that does not implement
a flag or an option rejects it; nothing is silently ignored.

### Address copies

Addresses cross the user boundary through `struct sockaddr_storage`
(128 bytes, 8 byte aligned) and `socklen_t`. An input address is copied
with its length checked against the family minimum and the storage size.
An output address is written up to the room the caller offered and the
full length is reported, as POSIX specifies for truncation. No backend
touches a user pointer.

### Packet ownership

A packet buffer (`struct pbuf`, N02) has exactly one owner at any time:
the pool, the stack (protocol code or a system call), a queue, or a device.
Each hand-over names the owner it expects and the owner it installs and
fails when the expectation is wrong. Application memory is never referenced
by asynchronous work or by DMA; data is copied into kernel buffers before
a request is queued. A buffer given to a device stays valid until the
device completes it or a completed reset ends its access.

### Worker requests

One kernel thread, `netd`, owns protocol state transitions, interface and
neighbour state and every protocol deadline. System calls submit bounded
requests and wait for their completion; the worker never blocks on a peer,
on application buffer space or on a device descriptor. Requests and input
packets are processed in bounded batches with the timers checked between
batches. Virtio completion callbacks only record completions and wake the
worker.

### Clock

Every deadline is taken from `net_clock_ms()` (`kernel/include/net/clock.h`),
which returns `timer_ms()` unless a kernel self test has taken control of
it with `net_clock_control(true)`. A controlled clock only moves through
`net_clock_set` and `net_clock_advance`, so a test drives retransmissions
and expiries without waiting. Releasing control returns to real time, which
may lie before the last controlled value; only tests release it, at their
end. Protocol code never reads `timer_ms()` directly.

### Errors

Kernel functions return negative errno values. The socket ABI adds the
values applications expect from a POSIX stack (`EDESTADDRREQ`,
`EPROTOTYPE`, `ENOPROTOOPT`, `ESOCKTNOSUPPORT`, `EADDRNOTAVAIL`,
`ENETDOWN`, `ENETUNREACH`, `ENETRESET`, `ECONNABORTED`, `ENOBUFS`,
`ESHUTDOWN`, `EHOSTDOWN`, `EHOSTUNREACH`, `EALREADY`, `EINPROGRESS`), with
the same numbers in the kernel and libc headers and a string in
`strerror`. An asynchronous error is stored in the socket and reported
once by `SO_ERROR`; a later operation that fails for the same reason
reports it again.

## Test infrastructure (N00)

Three layers of tests are kept apart and the layer of every result is
named with it: kernel self tests of the protocol code alone (`net_*`
cases running `test=net_*`), boot tests with a controlled host peer, and
later interoperability runs against real peers.

### Kernel seams

The controlled clock above is the first seam. The second is packet
injection: the worker's input entry and the loopback interface accept
packets from a test, and the IP input handler is replaceable
(`net_set_ip_input`, N02), so protocol logic is exercised without a PCI
device and without user pointers.

### Peer harness

`tests/run_qemu_test.sh` reads two more case files:

- `nic` names the backend of a `virtio-net-pci` device: `dgram` exchanges
  raw Ethernet frames as UDP datagrams with a host peer on 127.0.0.1,
  `user` attaches QEMU's user mode stack (for the DHCP and DNS milestones),
  `none` attaches nothing. The backend must be offered by the QEMU binary
  in use; the harness checks `-netdev help` rather than assuming it. The
  device gets the fixed address `52:54:00:4d:49:4f` and every frame is
  captured to `<out>/capture.pcap` through a `filter-dump` object.
- `peer` is an executable started before the image is built when the
  backend is `dgram`. It receives `NETPEER` (the host tool), `PEER_READY`,
  `PEER_LOG`, `PEER_PID`, `OUTDIR`, `TOP` and `BUILD`, and it must write
  `<peer port> <guest port>` to `PEER_READY` once it listens; the harness
  waits up to ten seconds for that file and starts QEMU with the dgram
  backend bound to the guest port and sending to the peer port. `PEER_LOG`
  and `PEER_READY` are passed on to the `post` script, together with
  `CAPTURE`.

The peer is stopped by the harness in every exit path (QEMU exited, the
timeout killed it, an earlier step failed): `SIGTERM`, five seconds of
grace, then `SIGKILL`, followed by `wait`, so no peer process and no port
reservation survives a failed case. The peer's log is complete before the
expectations and the post script run.

`tools/netpeer/netpeer.c` is the peer used by the cases, built to
`build/host/netpeer`. It binds its UDP socket on an ephemeral port, finds a
free port for QEMU by binding and releasing one, publishes both, logs one
line per frame (timestamp, length, the Ethernet header) and a `summary`
line on termination. The modes are `count` and `echo`; `--probe PORT` tells
whether a port is free, which the self test uses. Later milestones add
responder modes to this tool rather than new peers, so the lifecycle stays
the one tested here. `tests/net/peer-count.sh` is the peer script a case
copies or calls.

`make check-net` runs `tests/net/selftest.sh`: with `tests/net/fake-qemu.sh`
standing in for QEMU it drives the harness through the success path, the
timeout path and an unknown backend, and checks the ports handed to QEMU,
the peer summary, the absence of the peer process and the release of its
port afterwards. `tests/cases/net_harness` boots the real kernel with the
device attached and the counting peer; the kernel has no driver for it yet,
so the case verifies that an unhandled virtio-net function does not disturb
the boot and that the peer lifecycle works with real QEMU. `boot` remains
the no-NIC reference.

`tools/run.sh` accepts `--nic BACKEND` (`QEMU_NIC` in `qemu.conf`): `none`
by default, `user`, or a complete `-netdev` argument without the id.

## The socket layer (N01)

`kernel/ipc/socket.c` holds the common layer, `kernel/ipc/unix_socket.c`
the Unix backend of M23 moved behind it, and `kernel/net/inet_socket.c`
the Internet family.

### Objects

`struct socket` carries the family, type and protocol, the backend
operation table (`struct socket_ops`), `error`, `poll` and `file`.
`socket_create` looks the family up in a small registration table
(`socket_register_family`), lets the family validate the type and protocol
and install the backend, and wraps the object in a file with the common
`sock_fops`. Backends allocate accepted sockets with `socket_alloc` and the
common layer wraps them. The file's release calls the backend's release
and frees the object; the poll source lives in the object, so a poller
registered on it before a connection was made or after it was closed
keeps receiving notifications until the descriptor is closed.

The Unix backend clears its pointer to the socket's poll source under the
connection lock when a side releases, and announces readiness under that
lock, which is why `conn.lock` sits above `poll_source.lock`
(`docs/design/locking.md`). The listener's source is notified by
`connect` while the listener is still in the name table, which its release
leaves first.

### ABI

`socket(AF_UNIX, SOCK_STREAM, flags)` keeps the M23 convention of flags
in the protocol argument; any other value there is `EPROTONOSUPPORT`.
`SOCK_NONBLOCK` and `SOCK_CLOEXEC` in the type are accepted for every
family. `socket(AF_INET, type, protocol)` validates the pair: a type other
than `SOCK_STREAM` or `SOCK_DGRAM` is `ESOCKTNOSUPPORT`, a protocol that
does not match the type is `EPROTONOSUPPORT`, and protocol 0 selects TCP
or UDP. Until N05 and N06 register their backends with
`inet_register_protocol`, valid pairs fail with `EPROTONOSUPPORT` as
well. `socketpair` is `AF_UNIX` only (`EOPNOTSUPP` elsewhere). Existing
numbers are unchanged; `getsockname` (87), `getpeername` (88),
`setsockopt` (89) and `getsockopt` (90) are appended.

`struct sockaddr_in`, `struct in_addr`, `struct sockaddr_storage`, the
`INADDR_*` and `IPPROTO_*` constants, the `MSG_*` flags and the `SO_*`
options are defined once in `minios/abi.h` and exposed by
`sys/socket.h`, `netinet/in.h` and `arpa/inet.h`. The byte order helpers
are inline in `netinet/in.h`.

### Addresses

`socket_addr_from_user` copies at most `sizeof(struct sockaddr_storage)`
bytes, requires at least the family word, looks the family's minimum up
(`socket_addr_min_len`) and rejects a length below it with `EINVAL` and an
unknown family with `EAFNOSUPPORT`; `bind` and `connect` then reject a
family other than the socket's. `socket_addr_to_user` reads the room the
caller offered, writes `min(room, length)` bytes and stores the full
length, so a caller can detect truncation. `accept`, `getsockname`,
`getpeername` and `recvmsg` with `msg_name` use it. An unnamed Unix
socket reports the family word alone (length 2), a named one the whole
`sockaddr_un`; the accepted socket carries the listener's name, and a
client's peer name is the listener's.

### Messages and flags

`sendmsg` and `recvmsg` pass their flags to the backend in a
`struct socket_msg` after the common layer masked them: a bit outside
`SOCKET_MSG_FLAGS`, or a receive-only flag on send and the reverse, is
`EINVAL`; a flag the backend does not implement is `EOPNOTSUPP`. Nothing
is ignored. `MSG_DONTWAIT` is added when the file is non blocking. The
Unix stream backend implements `MSG_DONTWAIT`, `MSG_NOSIGNAL`, `MSG_PEEK`
(a copy that consumes nothing and ignores descriptor records) and
`MSG_WAITALL` (the read continues until the request is full, the peer
closes, a signal arrives after some data, or a message carrying
descriptors begins); `MSG_OOB`, `MSG_EOR` and `MSG_TRUNC` are rejected.
libc's `send`, `recv`, `sendto` and `recvfrom` are the message calls with
one buffer, so flags reach the kernel; `read` and `write` on a socket are
the message calls with no flags.

The message header path checks every iovec element against a 1 GiB
limit and the running sum against the same limit before any range is
looked at, so the total cannot overflow; at most 32 elements are accepted.
A stream call moves at most 64 KiB and reports the partial count; a
datagram above it will be `EMSGSIZE`. A control message must be one
complete `SCM_RIGHTS` record with a whole number of descriptors on an
`AF_UNIX` socket; anything else is `EINVAL` (`EOPNOTSUPP` on other
families) and no descriptor is looked up. Descriptors already referenced
are released on every later failure.

### Position independent I/O

`struct file_ops.flags` carries `FOPS_STREAM`; `file_read` and
`file_write` call such an object without taking `file.lock`, which
regular files keep for their position. A thread blocked in `read` on a
socket therefore no longer excludes a `write` on the same open file
description, which `sockets_api` demonstrates with a reader thread and a
writer on one descriptor of a pair.

### Options and errors

The common layer answers `SO_TYPE`, `SO_DOMAIN`, `SO_PROTOCOL` and
`SO_ERROR` (which consumes the pending error recorded by
`socket_set_error`) and refuses to set them; other options go to the
backend, which for Unix sockets is `ENOPROTOOPT`. Option values are at
most 64 bytes.

### Readiness

Poll reports, per state: an unbound or unconnected stream `POLLHUP`; a
listener `POLLIN` with a pending connection; a connecting endpoint (from
N06) nothing until the handshake ends, then `POLLOUT` or
`POLLERR | POLLHUP` with the error in `SO_ERROR`; an established endpoint
`POLLIN` with queued data or a closed peer and `POLLOUT` with room or a
closed peer; both directions ended `POLLHUP` as well.

### Tests

`sockets_api` (`/bin/sockapi`) covers the blocked reader with an
independent writer, every flag above, address lengths, truncation,
unnamed and named peers, family and type errors, iovec overflow and range
errors, malformed control records without a leaked descriptor, and the
options. `net_socket` runs two hundred listen, connect, accept, send,
receive and release cycles through the kernel API, with the error paths,
and checks that the free page count returns to its settled baseline.
`sockets`, `evfd`, `fdflags`, `pthreads`, `poll_wake`, `pipes` and the
compositor cases `comp_core`, `comp_data`, `comp_seat`, `comp_panel` and
`gui_app` are the regressions.

## Limits

Public Internet services are never test dependencies. The dgram backend on
this host (QEMU 11.0, macOS) delivers one frame per datagram in both
directions; a backend on another host must be checked the same way before
its results are trusted.
