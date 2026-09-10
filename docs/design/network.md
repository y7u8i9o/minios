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

## Limits

Public Internet services are never test dependencies. The dgram backend on
this host (QEMU 11.0, macOS) delivers one frame per datagram in both
directions; a backend on another host must be checked the same way before
its results are trusted.
