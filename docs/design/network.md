# Networking

The TCP/IP stack is built in the `N` milestones of `NETWORK_PLAN.md`. This
document describes the N00 contracts, common socket layer (N01), worker and
packet core (N02), VirtIO Ethernet (N03), IPv4/ARP/ICMP (N04), UDP (N05),
and TCP connection lifecycle (N06). Later milestones extend it.

## Feature matrix

The initial target is IPv4 host operation on QEMU's q35 machine with one
modern virtio-net device and the loopback interface. Checksums are computed
in software; no segmentation or checksum offload is negotiated. IPv6,
forwarding, multicast, raw sockets for applications, other NIC drivers and
multiple queue pairs are outside the plan. The specifications consulted
are recorded here as their milestones land:

| Area | Specification | Supported subset |
|---|---|---|
| Packet checksums | RFC 1071 | one's complement sum over 16 bit words, odd trailing byte padded with zero, accumulated over even pieces (N02) |

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
device attached and the counting peer; the case was originally the no-driver boot check; after N03 it
verifies that an attached, unconfigured NIC does not disturb the boot and that the peer lifecycle works with real QEMU. `boot` remains
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
or UDP. N05 registers UDP with `inet_register_protocol`; TCP remains
`EPROTONOSUPPORT` until N06. `socketpair` is `AF_UNIX` only (`EOPNOTSUPP` elsewhere). Existing
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

## The packet core (N02)

`kernel/net/` holds `pbuf.c` (the pool), `checksum.c`, `netif.c` (the
interface table and the two data paths), `loopback.c`, `worker.c` (the
worker, its requests and its timers), `net.c` (initialization and the IP
entry point) and `clock.c` (N00). `net_init` runs from `kinit` after the
daemons it resembles: the pool, the interface table and the worker come
up first, then the loopback interface is registered and marked up, so no
interface exists before the worker that serves it.

### Packet buffers

The pool is 256 buffers of 8192 bytes carved from one buddy block at
initialization (2 MiB), never grown. A buffer starts with 64 bytes of
headroom for the headers lower layers push in front of a payload and
leaves 8128 bytes of capacity, enough for an Ethernet frame with the
virtio header and, since N08, for the largest reassembled IPv4 packet
(`IPV4_MAX_PACKET`). N02 through N07 used 2048-byte buffers. `pbuf_push`, `pbuf_pull`, `pbuf_put` and `pbuf_trim` move
the data window and fail instead of leaving the buffer; wire headers are
read through the unaligned accessors of `net/byteorder.h`.

Each buffer records its owner: the pool, the stack (protocol code or a
system call), a queue (the worker's input queue) or a device (a virtqueue
descriptor, from N03). `pbuf_alloc` and `pbuf_free` move buffers between
the pool and the stack; `pbuf_transfer(p, from, to)` moves between the
other owners and fails with `EINVAL`, counted in `bad_transfer`, when the
buffer is not owned by `from`. A device hand-over therefore cannot be
undone twice, a queued buffer cannot be queued again, and a buffer a
device still owns cannot be freed. `pbuf_free` of a buffer the stack does
not own is a kernel assertion.

Thirty two buffers are a reserve: `pbuf_alloc(PBUF_DATA)` fails once
only the reserve is left, `pbuf_alloc(PBUF_CONTROL)` may take it. Data
traffic under pressure therefore leaves room for ACKs, teardown and
neighbour replies. Failures and the low water mark are counted
(`pbuf_get_stats`). Allocation failure on an output path frees nothing
and returns `ENOBUFS` to the caller; on an input path the driver leaves
the frame in its ring.

### Checksums

`net_checksum_partial`, `net_checksum_finish` and `net_checksum` compute
the RFC 1071 sum over unaligned bytes, fold the carries at every piece so
a run of pieces cannot overflow, and pad an odd trailing byte with zero.
Every piece but the last must have an even length. The tests use the RFC
example, an odd length datagram that verifies to zero, the all ones
carry case and the empty input.

### Interfaces

`struct netif` has a name, an index, flags (`NETIF_UP`, `NETIF_LOOPBACK`,
`NETIF_ETHERNET`), an MTU, a hardware address, an operation table with
`output` and an optional link layer `input`, and relaxed atomic counters
(packets, bytes, drops and errors per direction). At most four interfaces
register; a duplicate name is `EEXIST`. `netif_output` refuses a down
interface with `ENETDOWN`, frees the buffer and counts the drop, and
otherwise hands the buffer to the driver, which consumes it either way.
`netif_input` marks the buffer with its interface, counts it and queues
it for the worker; a full queue is `ENOBUFS`, the buffer is freed and the
drop is counted on the interface and the worker. The worker delivers a
dequeued buffer to the interface's `input`, or without one to
`net_ip_input`, which runs the handler installed by `net_set_ip_input`
(IPv4 from N04, a test handler until then) and otherwise uses the IPv4 parser added in N04.

`netif_set_up(n, false)` clears the flag and drains the worker, so on
return no packet of the interface is being processed; packets still
queued for it are dropped when the worker reaches them and counted as
`packets_dropped_down` and on the interface. `netif_unregister` takes the
interface down first. `lo` is the loopback interface: its output queues
the buffer as its own input, so a packet to the host takes the path a
received frame takes; its MTU is the buffer capacity, 1984 bytes.
`netif_format_table` prints one line per interface with the counters, the
basis of the later configuration utility.

### The worker

`netd` is a kernel thread at the highest scheduler level. Under
`net_worker.lock` it keeps the input queue (at most 128 buffers), the
request queue (at most 64) and the armed timers sorted by deadline. Its
loop takes the lock, checks whether a packet, a request or a due timer
exists or a kick arrived, and otherwise sleeps on its wait queue with the
lock as condition lock, using `waitq_wait_timeout` until the earliest
deadline when the clock is real and an unlimited wait when a test
controls the clock (a controlled deadline is reached only when the test
moves time and calls `net_worker_kick`). Producers enqueue and wake under
the same lock, so work arriving between the check and the sleep is seen.
Every pass then fires the due timers, processes at most 32 packets, then
at most 32 requests, and checks the timers again, so a flood of input
cannot postpone a due timer past one batch; the test measures the
distance in packets and requires at most two batches.

A request (`struct net_request`) carries a function the worker runs with
no lock held; the caller either waits (`net_request_wait`, which returns
the function's result) or sets `done`, which the worker calls last and
which may free the request. `net_request_submit` fails with `ENOBUFS` on
a full queue and counts the rejection. A queued request can be withdrawn
(`net_request_cancel`, once); a waiter whose process is exiting or has a
pending signal withdraws its queued request and returns `EINTR`, while a
request the worker has taken is always waited for, which is bounded
because the worker does not block. `net_worker_drain` runs an empty
request and is the synchronization point interface changes use.

A timer (`struct net_timer`) is a one shot deadline on the network clock
whose function runs on the worker and may re-arm it. `net_timer_arm`
inserts in deadline order and wakes the worker when the new timer is the
earliest, so a deadline already reached fires at the next pass;
`net_timer_cancel` returns whether the timer was armed, which is false
for one whose function is running or has run, and a caller that must know
the function is over synchronizes with a request.

### Tests

`net_core` covers: the pool emptied through the reserve with counted
failures and refilled; the ownership transitions including the refused
ones; the data window operations at their limits; the checksum vectors;
loopback delivery to the default and to a test entry point with an MTU
sized packet; the input queue filled to its limit with the worker held
(refused buffers freed and counted), the request queue filled with
rejections counted, cancellation of a queued and of a finished request,
completion of everything queued once the worker is released; an
interface taken down with packets queued (dropped and counted, output
refused with `ENETDOWN`, output again after up); timers that stay quiet,
fire when due without a kick, fire in deadline order after the controlled
clock moved, move when re-armed, re-arm themselves, fire within two
batches under a flood, and fire on the real clock without a kick; four
threads running 1200 requests with the worker sleeping without a timeout,
which would hang on a lost wakeup; and fifty cycles of packets, requests
and timers after which the pool is full and the queues are empty.

## Limits

Public Internet services are never test dependencies. The dgram backend on
this host (QEMU 11.0, macOS) delivers one frame per datagram in both
directions; a backend on another host must be checked the same way before
its results are trusted.

## VirtIO Ethernet (N03)

`virtio_net.c` discovers one PCI network function exposing modern capabilities.
It requires `VERSION_1` and `MAC`, validates the device-configuration length
and a nonzero unicast MAC, and reads the MAC across a stable configuration
generation. It negotiates no other feature. In particular, `STATUS` is not
negotiated: administrative up assumes a usable link. There is no claim of
physical link-change detection. Device-needs-reset or a broken queue stops the
interface; automatic restart and hot removal are outside this stage.

The driver uses the modern 12-byte network header, one RX queue and one TX
queue. Flags and GSO type must be zero. VirtIO 1.2 specifies an RX
`num_buffers` of 1 without `MRG_RXBUF`; QEMU 11.0 on the tested host actually
writes 0. The driver accepts 0 or 1 in this non-merged mode and rejects larger
values. Unused header hints are ignored. This compatibility exception is
covered by the simulated header tests and the live captures.

### DMA ownership and limits

There are 32 dedicated RX slots, each 1526 bytes including the VirtIO header,
and 32 TX slots holding device-owned packet buffers. Four TX slots are
available only to ARP/ICMP control traffic. RX does not pin buffers from the
shared packet pool. A completion callback records its slot and used length;
it does not process protocols, allocate, free, or refill descriptors.

Netd services completions between timer and packet batches. It copies a valid
RX frame into a shared packet buffer, hands that buffer to the input queue,
and refills the DMA slot after the transport has released its descriptor.
When the pool has no suitable buffer, the completed RX slot remains held and
a worker timer retries after 10 ms. ARP and ICMP may use the control reserve
on receive as well as transmit. Invalid lengths/headers are dropped and
counted without accessing bytes beyond the completed buffer.

A TX buffer remains `PBUF_OWNER_DEVICE` until its completion is serviced or
reset has demonstrably ended DMA. Full TX slots or descriptor exhaustion
return `ENOBUFS` and consume/free the proposed output packet. No output call
waits for device space. Completion tokens refer to stable TX slots, so reuse
of a descriptor cannot overwrite an unprocessed completion.

The shared transport now tracks published head descriptors and validates the
full 32-bit completion ID before narrowing it. An out-of-range ID, duplicate
completion, or used-index advance beyond the queue capacity marks the queue
broken instead of indexing outside its arrays. IRQ traversal is serialized
with queue detachment. Successful reset ends DMA before rings and driver
storage are released; a reset timeout deliberately retains potentially
DMA-visible memory. The static device object outlives its IRQ registration.
The four `net_nic_fail_*` cases inject failure after RX queue allocation, both
queues, RX-buffer publication, and `DRIVER_OK`, respectively, and check the
actual cleanup path before interface publication.

## Static IPv4, ARP and ICMP (N04)

Protocol state belongs to netd. `net_configure(interface, address, mask,
gateway)` is the kernel control boundary: it copies a bounded request to the
worker. Its IPv4 numbers are host order; application `sockaddr_in` fields
remain network order. Socket creation never changes interface configuration.
Ethernet starts up with no IPv4 address. Static test configuration is supplied
explicitly; DHCP and persistent user configuration remain N10 work.

### Routing and neighbors

There is one configured Ethernet interface, its connected route, an optional
default gateway on that subnet, and the loopback `127.0.0.0/8` route. Traffic
to the Ethernet interface's own address also takes the common loopback input
path. Subnet and broadcast destinations are refused. A default route resolves
the gateway's MAC, retaining the remote destination in the IPv4 header.
Missing routes return `ENETUNREACH`, a down output interface `ENETDOWN`.

ARP has 16 entries, at most four pending packets per neighbor and 32 pending
packets globally. Cache entries expire after 60 seconds; unresolved entries
send at most three requests at one-second intervals. Exhaustion returns
`ENOBUFS`. Timeout releases every pending packet and reports `EHOSTUNREACH`
to matching connected UDP sockets. A down interface fails its pending work
at the next neighbor deadline; reconfiguration flushes it with `ENETRESET`.
No asynchronous record retains an endpoint pointer.

ARP checks Ethernet/IPv4 types, hardware/protocol lengths, opcode, sender MAC,
source subnet and target address. Ethernet dispatch checks the ARP sender MAC
against the enclosing frame. Only existing neighbors are learned/refreshed;
requests for the local address still receive a reply without allocating a
cache entry. Unsolicited responses cannot fill the table. Proxy ARP is not
supported. N04 had no address-conflict detection and did not send ARP
probes with source address zero; N16 added both for the DHCP client.

### IPv4 and ICMP subset

IPv4 validates version, header length, total length, checksum, TTL and local
unicast destination. Ethernet padding is trimmed before upper-layer parsing.
The stage accepts a 20-byte header only, rejects options and all fragments,
and counts those restrictions. Output sets DF and refuses packets above the
interface MTU: 1500 on Ethernet and 1984 on loopback. Ethernet multicast,
IP broadcast, forwarding and raw application sockets are not provided.

ICMP validates its checksum, answers echo requests and counts received echo
replies. Unsupported IP protocols generate protocol-unreachable; an
unmatched UDP port generates port-unreachable. Errors quote the original IP
header and up to eight payload bytes. Errors about ICMP errors and replies
to non-unicast/invalid input are suppressed. Echo and error replies share a
limit of 20 per one-second window. Incoming destination-unreachable and
time-exceeded messages may set a connected UDP socket error after validation
of the quoted packet and its complete tuple. Redirects and source-quench
messages do not alter route or protocol state.

## UDP sockets (N05)

`AF_INET/SOCK_DGRAM` selects UDP for protocol 0 or `IPPROTO_UDP`. The endpoint
table has 64 slots. A socket receive ring holds at most 16 datagrams and
16384 payload bytes, subject to the shared packet pool. Queue overflow drops
the new datagram and increments `udp_full`. Closing removes the endpoint on
the worker with an uninterruptible lifetime barrier and releases unread
packets. Duplicated descriptors retain the endpoint until the last reference
closes. Sends retain kernel copies only, and the file reference remains held
until the worker request finishes or is withdrawn before execution.

### Binding and messages

Wildcard bindings exclude all specific addresses on the same port. Specific
local addresses may share a port when they differ. There is no reuse option;
unsupported options return `ENOPROTOOPT`. Port 0 selects the first free port
from a rotating 49152–65535 range. This is deterministic development behavior;
unpredictable allocation is deferred to N09.

Unconnected sockets use `sendto`/addressed `sendmsg`; an unaddressed send
returns `EDESTADDRREQ`. Connect records a default peer and local source and
filters subsequently arriving datagrams by peer address/port and local
address. Addressed sends may override the default destination. Reconnect
changes the filter; previously accepted receive-queue entries remain queued.
`AF_UNSPEC` disconnect and UDP shutdown are unsupported in this stage.

UDP length must match the IPv4 payload exactly. A nonzero checksum is
validated over the IPv4 pseudo-header and datagram; IPv4 UDP checksum zero is
accepted. Output always computes a checksum, transmitting a computed zero as
`0xffff`. One send produces one complete datagram or an error, with no partial
send. The maximum payload is 1472 bytes over Ethernet and 1956 over loopback;
fragmentation/reassembly is deferred to N08.

Empty datagrams occupy receive-ring entries and make `poll` readable.
`recv` with a zero-sized buffer consumes the next datagram unless `MSG_PEEK`
is specified, returning zero; it waits or returns `EAGAIN` when none exists.
The existing VFS `read(..., 0)` and `write(..., 0)` return zero without entering
the socket backend. Use `send`/`sendto` to transmit an empty datagram.
Nonzero Internet socket read/write operations bounce through kernel storage,
so no worker request or endpoint-lock copy accesses application memory.

A short receive copies the available prefix and discards the remainder of
that datagram. `recvmsg` sets output `MSG_TRUNC`. Input `MSG_TRUNC` returns the
original length while copyout remains bounded by the supplied iovecs.
`MSG_PEEK` leaves the datagram queued. Send implements `MSG_DONTWAIT` and
`MSG_NOSIGNAL`; receive implements `MSG_DONTWAIT`, `MSG_PEEK` and `MSG_TRUNC`.
Other known flags return `EOPNOTSUPP`, and invalid directional/unknown flags
are rejected by the common layer. Ancillary descriptor passing stays Unix-only.

### Readiness and errors

Datagrams are readable even when their payload is empty. An endpoint is
writable when open: a subsequent send may still fail with `ENOBUFS` under
bounded global request, packet or device pressure. Receive wait registration,
queue inspection, producer wakeup and poll notification use the same endpoint
condition lock. The worker never waits for a receiver.

Connected sockets map ICMP port-unreachable to `ECONNREFUSED`, network-
unreachable to `ENETUNREACH`, fragmentation-needed to `EMSGSIZE`, and other
supported unreachable/time-exceeded messages to `EHOSTUNREACH`. A pending
error sets `POLLERR`, wakes poll and receive waiters, and is consumed once by
`SO_ERROR`, the next send, or a receive with an empty queue. Kernel storage is
negative errno; `SO_ERROR` reports the positive value. Unconnected sockets
have no asynchronous error delivery. This tuple-based development policy is
not a spoof-resistant ICMP validation claim; N09 remains required.

## Specifications and N03–N05 evidence

Wire layouts and the supported subset were checked against
[VirtIO 1.2, section 5.1](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
[RFC 826](https://www.rfc-editor.org/info/rfc826/),
[RFC 791](https://www.rfc-editor.org/info/rfc791/),
[RFC 792](https://www.rfc-editor.org/info/rfc792/),
[RFC 768](https://www.rfc-editor.org/info/rfc768/), and
[RFC 1122, sections 3 and 4.1](https://www.rfc-editor.org/info/rfc1122/).
The omitted features and the QEMU header exception above are deliberate
restrictions, not claims of complete compliance with those specifications.

The deterministic device tests live in `test_net_device.c`, IPv4/ARP/ICMP
tests in `test_net_ipv4.c`, and UDP tests in `test_net_udp.c`. Shared builders
in `net_helpers.h` prepare input; validation and delivery use production code.
`netudpapi.c` tests the application ABI and concurrent operations. The peer
harness additionally accepts an executable peer for `nic user`, passing its
ephemeral native UDP port in the guest command line. All peers retain the
same readiness, success, timeout and cleanup lifecycle.

`net_nic` checks 640 raw-frame RX/TX exchanges and reset accounting;
`net_virtqueue` checks 66000 completions across 16-bit index wrap, a full ring,
burst reclamation, invalid/duplicate IDs, used-index overrun, and invalid
network headers. `net_icmp` uses the isolated raw peer in both directions.
`net_udp_peer` exchanges 300 datagrams with the host's native UDP socket via
QEMU user networking, cycling through empty, odd-length and MTU-sized
payloads. Its capture contains 600 UDP packets whose IPv4 and UDP lengths
and checksums are verified independently by `check_capture.py`.


## TCP connection lifecycle (N06)

`AF_INET/SOCK_STREAM` selects TCP for protocol 0 or `IPPROTO_TCP`. The
implementation is divided by responsibility in `kernel/net/tcp`: `core.c`
owns connection objects and published state, `input.c` parses and processes
segments, `output.c` constructs packets, `socket.c` implements application
operations, and `timer.c` owns deadlines. The wire format and state transitions
were checked against [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html).
This stage implements connection lifecycle with bounded small exchanges.
It does not claim the reliable bulk-transfer behavior reserved for N07.

### Connections, queues and ownership

There are separate fixed tables of 64 application endpoints and 64 connection
control blocks. Netd owns protocol state, sequence numbers, listener membership
and timers. `tcp_lock` protects endpoint snapshots, readiness, wait conditions
and the 4096-byte receive ring of each connection. Socket requests retain kernel
copies and a live file reference. Copies under the lock are bounded and never
access application memory.

Active open reserves a port and publishes SYN_SENT before transmitting SYN.
Passive open creates a SYN_RECEIVED child in a separate half-open queue. Only a
validated final ACK promotes it to the FIFO accept queue. Listen backlog is
clamped to 1–16. The half-open limit is the smaller of backlog and 8, while the
completed queue independently allows backlog children. Excess SYNs are dropped.
A completed handshake that cannot enter the accept queue is reset and released.
Unaccepted established children expire after 30 seconds.

Accept transfers a child from its listener to a new endpoint. Closing a listener
resets and releases its queued children, while previously accepted sockets
remain usable. Final socket-file close waits uninterruptibly for a worker
request to remove the endpoint. FIN processing and TIME_WAIT may retain a
connection afterward, with no pointer to the freed endpoint or socket.
Device packets contain copies of bytes, never pointers to connection state.

Binding follows the UDP wildcard/specific-address collision rules in a separate
TCP port namespace. Closing connections and TIME_WAIT continue to reserve their
local port. There is no SO_REUSEADDR bypass. Ephemeral ports are drawn
from 49152–65535. Sequence and port generators can be replaced on netd for
tests. Since N09 the default generators take their values from the kernel
random provider and fail closed while it is not ready (below).

### Connect, readiness and errors

Blocking connect and accept sleep on endpoint conditions without blocking netd.
Nonblocking connect returns EINPROGRESS, and a repeated pending connect returns
EALREADY. Completion publishes POLLOUT for success or failure. Applications must
read SO_ERROR to distinguish them. A reset acknowledging the outstanding SYN
reports ECONNREFUSED. Exhausted handshake retries report ETIMEDOUT. Errors are
negative internally and positive through SO_ERROR, which consumes its pending
value once. Subsequent operations can still return the terminal connection
error. An exact tuple and original SYN sequence are required before a quoted
ICMP or local ARP failure aborts an active handshake.

Signals interrupt connect or accept waits with EINTR. An interrupted connect
that has already started leaves its connection in progress, so the application
can use poll/SO_ERROR or close it. A request interrupted before worker execution
is withdrawn through the common request mechanism. Descriptor duplication and
process exit use ordinary file references and the same final-close barrier.

Established sockets become readable for buffered data, read shutdown or EOF.
They become writable while the send buffer of N07 has room, independent of
the peer window and the congestion window. Poll cannot promise that a later packet allocation
will succeed. A bounded-resource failure can still return ENOBUFS.

### Parsing and the N06 data boundary

Input verifies the TCP pseudo-header checksum, minimum header, data offset,
reserved header bits and option bounds. MSS is accepted only as one valid,
nonzero, four-byte SYN option. Malformed TLVs are discarded. Well-formed unknown
options are skipped. SYN plus FIN and urgent data are unsupported and dropped.
N06 advertised no option but MSS; window scaling and timestamps arrived in
N13 and SACK in N14, while ECN and TCP Fast Open remain absent.
SYN payload is not acknowledged or delivered during this stage.

Outgoing SYNs advertise the smaller of 1460 and route MTU minus 40. Peer MSS
limits each segment, with a default of 536 when absent. Sequence comparisons
use modular 32-bit arithmetic and are tested across wraparound. N06 held one
outgoing segment at a time and stored only in-order receive data; the send
buffer, the out-of-order store and the recovery rules of N07 replaced that
boundary. Reading reopens the window through a worker ACK request.

Receive supports MSG_DONTWAIT and MSG_PEEK. Send supports MSG_DONTWAIT and
MSG_NOSIGNAL. Unsupported message modes, including MSG_WAITALL and urgent data,
fail explicitly. A send after write shutdown returns EPIPE and raises SIGPIPE
unless MSG_NOSIGNAL is used. Ancillary file passing remains Unix-only.

N06 reset a connection whose data went unacknowledged for 10 seconds and
could exchange small payloads only on lossless paths. Data retransmission,
out-of-order storage, congestion control, zero-window probes and the
progress deadline that replaced that limit are described under N07.

### FIN, reset and deadlines

A FIN consumes one sequence number after its segment's data. Only in-order data
and a FIN within the receive window advance the receive sequence. EOF follows
all queued bytes. SHUT_WR waits for outstanding data acknowledgement before
sending FIN, while the receive direction remains open. SHUT_RD discards queued
data, returns EOF, and advertises the reopened window. Crossing FINs use CLOSING
before TIME_WAIT. Peer-first close uses CLOSE_WAIT and LAST_ACK.

Outside SYN_SENT, an out-of-window RST is ignored. An in-window RST with a
sequence different from RCV.NXT elicits a rate-limited challenge ACK. An exact
RST terminates the connection and wakes waiters. Unexpected SYNs and future
ACKs cannot advance established sequence space. Reset replies and challenge
ACKs share a limit of 20 per second. Closing with unread data resets the peer.
Data arriving after final file close also resets the detached connection.

SYN, SYN ACK and FIN retain their original sequence for retransmission. The
initial retry delay is one second, followed by exponential backoff and at most
three retransmissions. With timely dispatch, retries occur at 1, 3 and 7 seconds
and failure at 15 seconds. Orphan teardown and FIN_WAIT_2 have 30-second bounds.
TIME_WAIT lasts 120 seconds, using a development MSL assumption of 60 seconds.
A duplicate peer FIN is acknowledged and restarts that deadline. RST cannot
remove TIME_WAIT. Port reuse becomes possible only after expiry and removal of
any remaining endpoint reservation.

All deadlines use the network clock and execute on netd. Pure TCP control
packets can use the shared packet and NIC descriptor reserves. This avoids
making FIN, ACK and reset transmission depend exclusively on data capacity.

### N06 validation

`net_tcp` injects controlled wire inputs into production IPv4/TCP and captures
production output through a fake Ethernet interface. It covers active/passive
and simultaneous open, lost and duplicate handshake packets, malformed headers
and MSS, queue exhaustion, refusal, timeout, poll/SO_ERROR, sequence wrap,
half-close, resets, crossing FINs, retransmitted FINs, orphan deadlines, port
retention and TIME_WAIT expiry. It also checks that connections, endpoints and
packet buffers return to baseline. `net_tcp_timer` advances the controlled clock
from outside netd and verifies actual worker dispatch at the TIME_WAIT boundary.

`net_tcp_api` runs the application ABI over loopback, including blocking and
nonblocking operations, short accept address copies, read/write, peek, poll,
half-close, signal interruption of accept, duplicate descriptors and process
exit with outstanding data. `net_tcp_peer` completes 20 native host TCP
connections over the real VirtIO NIC and QEMU user backend. The host waits for
the guest's FIN before echoing the payload and closing its write direction.
The capture post-check verifies lengths and IPv4/TCP checksums independently.
The native host peer tests guest active open, while controlled injections and
loopback tests cover guest passive open.

Exact commands, results and retained artifacts are recorded in
`docs/design/network-n06-validation.md`.


## TCP reliable transfer (N07)

N07 turns the lifecycle of N06 into a stream. `transfer.c` owns the send
buffer, acknowledgement accounting, round-trip estimation and congestion
control; `receive.c` owns ordered delivery and the out-of-order store. Both
run only on netd, and both keep the rule that device packets are copies:
retransmission copies from the send buffer into a fresh packet, and device
completion never acknowledges TCP sequence space. The behaviour follows
[RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html) for the stream,
[RFC 6298](https://www.rfc-editor.org/rfc/rfc6298.html) for the
retransmission timer and the Tahoe subset of
[RFC 5681](https://www.rfc-editor.org/rfc/rfc5681.html) for congestion
control. N07 advertised no window scaling, SACK, timestamps or ECN, so its
receive window was limited to a 4096-byte ring and the peer's window was
taken at face value; N13 and N14 changed that and are described below.

### Send buffering and acknowledgement

Each connection holds a send buffer of `TCP_SEND_CAPACITY` bytes (8192 in
N07, 65536 since N13). A send
copies as many bytes as fit and returns that partial count; a full buffer
returns `EAGAIN` to nonblocking callers and blocks the others until an
acknowledgement frees space. The prefix `transmit_sent` is on the wire and the
whole buffer stays owned by the connection until a cumulative ACK covers it or
a terminal error discards it. `tcp_flush` sends at most eight new segments per
call, each at most the peer MSS, up to the smaller of the congestion window
and the peer window; a packet allocation failure keeps the bytes and retries
at the next ACK or deadline instead of dropping them.

ACK processing (`tcp_data_ack`) ignores acknowledgements outside
`[snd_una, snd_nxt]`, releases the acknowledged prefix with one `memmove`
across the buffer, and clears the duplicate and retry counters. A partial ACK
therefore retains the exact unacknowledged suffix, tested across sequence
wraparound. A pure ACK that repeats `snd_una` with the same window while data
is outstanding counts as a duplicate; the third duplicate triggers fast
retransmit. The window update rule from N06 (`snd_wl1`/`snd_wl2`) is
unchanged.

### Ordered receive and the out-of-order store

One circular store (4096 bytes in N07, 131072 since N13) holds both readable
bytes and bytes that arrived ahead of a hole. A presence bitmap marks the out-of-order positions; only the
contiguous prefix counts toward `receive_count`. N07 subtracted the
out-of-order bytes from the advertised window; since N13 they are stored
inside the window without shrinking it (see N13). Bytes are accepted only inside
`[rcv_nxt, rcv_nxt + window)`; the first arrival of a byte wins, so an
overlapping retransmission with different content cannot change a byte
already accepted, and no byte is delivered twice. When a hole is filled the
loop advances `rcv_nxt` across every present byte. A FIN whose sequence lies
beyond a hole is retained (`pending_fin`) and consumed only when the bytes
before it have arrived, so EOF still follows all data. Read shutdown keeps
the out-of-order positions but discards the contiguous bytes by moving the
ring origin, which keeps the sequence-to-slot mapping intact.

In N07 every segment that carried data or a FIN was acknowledged at once.
N14 replaced that with delayed ACKs within the rules of RFC 1122 (see N14);
duplicates, out-of-order segments and FINs are still acknowledged at once.
There is no Nagle algorithm, so a small write is sent as soon as the
windows allow and a sender never waits for the receiver's delayed-ACK timer
before sending.

### Retransmission, round-trip time and congestion control

The retransmission timeout starts at one second. One unambiguous sample is
taken at a time: a timer starts when the buffer was empty and no recovery is
in progress, and the sample is complete when an ACK covers its end. Samples
are clamped to 1–60000 ms and smoothed with the RFC 6298 estimator (`srtt`
gain 1/8, variance gain 1/4); the timeout is `srtt + 4 * rttvar` bounded to
1–60 seconds. Karn's rule applies: any retransmission cancels the sample in
progress, so an acknowledgement that may belong to the retransmitted copy is
never measured.

The congestion window starts at one MSS with a 65535-byte slow-start
threshold. Slow start adds one MSS per acknowledged MSS; congestion avoidance
accumulates acknowledged bytes and adds one MSS per full window. The window
never exceeds the send buffer. Loss, whether a timeout or three duplicate
ACKs, halves the outstanding data into the threshold (at least two MSS),
returns the window to one MSS, and enters recovery until `snd_nxt` at the
time of the loss is acknowledged; recovery suppresses a second fast
retransmit for the same loss. A timeout retransmits one segment from
`snd_una`, doubles the timeout up to 60 seconds and counts a retry. When the
buffer has been idle for one timeout the window is reset to one MSS before
the next burst.

The retry policy is explicit. Unacknowledged data is failed with `ETIMEDOUT`
and a reset after `TCP_DATA_RETRIES` (8) consecutive timeouts or after the
progress deadline `TCP_PROGRESS_MS` (120 seconds) without any acknowledged
byte, whichever comes first. The progress deadline is renewed by every
acknowledgement that frees data, so a slow but live peer is never failed. The
same timer serves the control retransmissions of N06, the data timeout and
the lifetime deadline; `tcp_schedule` arms it for the earliest one.

### Zero windows, small writes and independence

When the peer advertises a zero window queued data stays in the buffer and
the data timer becomes a probe timer. Each expiry sends one byte at
`snd_una - 1`, a sequence the peer has already consumed, so the byte cannot
enter its stream while its acknowledgement reports the current window. The
probe backs off like a retransmission and the progress deadline still bounds
the whole stall. A window update is applied by the ordinary ACK rules and
`tcp_flush` resumes at once, tested with a lost window-opening ACK.

Readiness and errors keep the N06 semantics. `POLLOUT` reflects buffer room,
not the peer window, so an application can fill the buffer while the peer
is stalled and blocks only when the buffer is full. A reader consumes from
the ring under `tcp_lock` and asks netd for a window update afterwards; a
failure of that request cannot turn a completed read into an error. Stalled
connections hold only their own buffers and timers: netd never waits for a
peer, and the pressure test sends on one connection while another has filled
its receive ring and a third waits in a zero window.

### N07 validation

`net_tcp_transfer` drives production TCP with injected segments and captured
output: a write larger than the buffer returns the partial count, the
congestion window limits the first burst below the buffer size, a partial ACK
across wraparound leaves the exact suffix, an RTT sample sets the timeout,
three duplicate ACKs enter recovery with the expected threshold and window
and suppress sampling, a timeout doubles the timeout and keeps the bytes, a
zero window retains data and is probed with `snd_una - 1`, and reordered,
overlapping and FIN-before-gap input delivers the exact stream. `net_tcp_bulk`
exchanges 262144 bytes in each direction with a native host TCP peer through
the VirtIO NIC and QEMU user networking; the peer echoes the stream, the
guest verifies every byte and EOF, and the capture check verifies the lengths
and checksums of every TCP packet. `net_pressure` covers the independence of
stalled connections. Controlled loss with a scripted peer is not part of the
suite; recovery is tested through the injected segments and the fake clock.

## IPv4 reassembly and path MTU (N08)

N08 removes the N04 fragment rejection and the N05 datagram-size limit on the
Ethernet path. Reassembly (`fragment.c`), the path MTU cache and the
recent-transmission table (`path.c`) belong to netd. Packet buffers grew to
8192 bytes so that a reassembled packet of `IPV4_MAX_PACKET` bytes (8128) is
one buffer, which keeps the ownership rules of N02. Fragmentation follows
[RFC 791](https://www.rfc-editor.org/info/rfc791/) and
[RFC 1122, section 3.3.2](https://www.rfc-editor.org/info/rfc1122/); path
MTU discovery follows [RFC 1191](https://www.rfc-editor.org/info/rfc1191/)
with the plateau table of its section 7, and the black-hole fallback is a
bounded version of [RFC 4821](https://www.rfc-editor.org/info/rfc4821/)'s
idea without its probing.

### Reassembly

A fragment is validated by `ipv4_fragment_bounds` before any copy: the
reserved flag, a zero length, a non-final fragment whose length is not a
multiple of eight, or an offset plus length beyond `IPV4_MAX_PACKET` minus the
header are rejected and counted in `fragment_invalid`. A reassembly context
is keyed by interface, source, destination, identification and protocol.
There are `IPV4_REASSEMBLY_SLOTS` (8) contexts, each owning one data buffer,
at most 64 fragments, and a deadline of `IPV4_REASSEMBLY_MS` (30 seconds)
that is set when the first fragment arrives and never renewed. A fragment
for a new datagram while every slot is busy is dropped and counted in
`fragment_full`; expired contexts are released by a worker timer and lazily
when a fragment arrives, counted in `fragment_expired`. Taking an interface
down flushes its contexts.

Overlap is rejected rather than resolved. A byte-granular presence bitmap
detects any fragment that covers a byte already present; an exact duplicate
(same offset, length, more flag and bytes) is ignored without effect on the
deadline, while a conflicting overlap, a fragment past a known end, a second
different end, or a 65th fragment poisons the context: its buffer is freed
and the key stays reserved until expiry, so a later fragment cannot
resurrect a datagram assembled from mixed sources. The reassembled packet
gets the first fragment's header with the total length, cleared fragment
fields and a fresh checksum, then enters the same protocol dispatch as an
unfragmented packet, so UDP checksum, port lookup and queue limits apply to
it unchanged.

### Outgoing fragmentation

Only UDP datagrams are sent without the DF flag. The UDP send limit is the
buffer capacity (`IPV4_MAX_PACKET` minus the UDP and IP headers) instead of
one MTU; a larger datagram still returns `EMSGSIZE`. `ipv4_link_output`
splits a packet larger than the path MTU after neighbour resolution, so ARP
queues one packet per datagram rather than its fragments. All fragment buffers
are allocated before the first one is transmitted; if the pool cannot supply
them the datagram is dropped with `ENOBUFS` and nothing partial reaches the
wire. A DF packet larger than the path MTU, a path below 68 bytes or more
than 64 fragments return `EMSGSIZE`. TCP never fragments: its segments are
sized from the path MTU and carry DF.

### Path MTU and ICMP correlation

`ipv4_note_output` records the first 28 bytes of every transmitted packet
(the header and eight transport bytes) with a 30-second expiry in a ring of
128 entries. An incoming ICMP destination unreachable or time exceeded
message is accepted only if its quote matches one of those entries in every
field except the TTL and the header checksum, which routers change. A quote
that fails is counted in `pmtu_rejected` and neither UDP nor TCP learns of
it, so a forged error needs the exact identification and sequence of a recent
packet. Fragmentation needed lowers the cached path MTU for the quoted
destination to the advertised next-hop MTU, or to the next plateau below the
quoted total length when the router reports none; a value below 68 or not
below the quoted length is rejected. The cache holds 16 destinations for ten
minutes each and the smallest of the interface MTU and the cached value is
used by TCP segment sizing, UDP fragmentation and the DF check. Lowering a
path shrinks the MSS and congestion window of every connection to that
destination and expedites its retransmission at the new size.

Replacing a route or address, and taking an interface down, flush both the
path cache and the correlation table, and the down path also fails every
connection on that interface with `ENETDOWN`.

### Missing feedback

When no ICMP arrives, TCP's data timeout provides the fallback. From the
second consecutive timeout with a nonzero peer window the connection lowers
the path MTU itself in steps (576, then 296 from the fourth retry, then 68
from the sixth), retransmitting smaller segments while the ordinary retry
count and progress deadline continue to run. The connection therefore either
recovers on a smaller path or fails with `ETIMEDOUT` in bounded time; it does
not probe upward again, and the lowered path expires with the cache.

### N08 validation

`net_fragment` reassembles a 3000-byte UDP datagram from reverse-order
fragments with an exact duplicate through production IPv4, UDP and socket
delivery, verifies that a conflicting overlap rejects the datagram and
quarantines its key, fills every context to check the global bound, rejects
an oversized offset before any copy, lets the real worker timer expire the
stale contexts under the controlled clock, and checks that valid traffic and
the packet pool recover. `net_path` verifies that a quote with one wrong
sequence byte is rejected, that the matching quote lowers the path and the
MSS, that retained data is retransmitted at the new size, that the cache
expires, that a black-hole path falls back to smaller segments and then fails
with `ETIMEDOUT`, and that a route replacement invalidates both tables.
`net_udp_peer` and the capture check cover unfragmented native traffic; a
fragmented exchange with a native peer is not in the suite because QEMU user
networking on this host does not forward fragments deterministically.

## Entropy, robustness and recovery (N09)

### The random provider

`kernel/lib/random.c` is the only source of unpredictable values in the
kernel. At `net_init`, before any interface is registered, it reads one
64-byte seed from a VirtIO entropy device (`virtio_rng.c`, device id 0x1044
or the transitional 0x1005) with a one-second bound. The device is used once:
its queue is polled without interrupts, short completions accumulate, the
device is reset before the buffer is freed, and a reset that does not
complete leaves the ring pinned rather than freeing DMA memory. A missing
device, a failed read or a constant seed leaves the provider unavailable and
logs one warning. There is no fallback to the timer, a fixed seed, the CPU
or libc `rand`; `random_u32` returns `EAGAIN` and `random_ready` reports the
state.

The generator is ChaCha20 (`chacha.c`, checked against the RFC 8439 test
vector by the host fuzzer) in a fast-key-erasure arrangement: each output
block replaces the key with its first 256 bits under `random_lock`, the
counter advances, and the block is erased. This gives forward secrecy of past
outputs from a captured state, not recovery after a compromise of the live
state, which would need reseeding that the single boot seed does not provide.

### Fail-closed policy

TCP initial sequence numbers and ephemeral TCP and UDP ports come from the
provider. While it is unavailable, connect, listen and passive open on TCP
and an automatic UDP port fail with `EAGAIN`; explicit binds, UDP sends from
bound ports, loopback and ICMP keep working. The kernel test seam
(`tcp_set_generators`) replaces the generators only on netd and only for the
duration of a test; it is not reachable from user space. The three
`net_random*` cases run the same kernel with a working device, no device and
a device that returns zeros.

### Input robustness

`tests/net/fuzz` builds the exact parsers and validators the kernel links
(`wire.c`, `checksum.c`, `socket_validate.c`, `chacha.c`) on the host with
the address and undefined-behaviour sanitizers and mutates inputs from a
seeded generator: random bytes, IPv4 headers with valid checksums and random
data offsets, TCP segments with valid pseudo-header checksums, fragment
flags with lengths near the limits, UDP headers, socket addresses and
`iovec` length arrays that overflow `size_t`. Accepted inputs are checked
for consistent bounds. `make check-net-fuzz` runs three seeds for 30 seconds
each; the run is reproducible from the printed seed. The runner prefers
Homebrew LLVM on macOS because the AddressSanitizer runtime of the Apple
toolchain recurses in its own startup on current beta releases, probes the
binary before fuzzing and falls back to UBSan alone when the address
sanitizer cannot start.

Inside the kernel the same parsers are reached only through the worker, in
bounded batches, after the driver validated the VirtIO header and frame
length. Reset replies, challenge ACKs, ICMP errors and echo replies share
per-second limits of 20, suppressed replies are counted, and a rejected
receive frame is logged once per boot. Interrupt handlers record completions
and wake netd; no packet is parsed and nothing is allocated in interrupt
context, so a flood costs one wakeup per batch.

### Pressure and recovery

`net_pressure` runs three identical cycles under the controlled clock, each
of which exhausts the 64 endpoints, floods a listener with 1000 SYNs and
checks the half-open bound, fills a receive ring with a slow reader while an
unrelated connection sends, allocates every data buffer and shows that a SYN
still leaves through the control reserve, takes the interface down under an
established connection and observes `ENETDOWN`, fills the connection table
with TIME_WAIT state that holds no socket files, and checks that a further
connect fails with `ENOBUFS`. After each cycle TIME_WAIT expires through the
real worker timer and the connection, endpoint and packet accounting return
to the baseline, with the packet low-water mark at the 32-buffer reserve.
`net_fragment` does the same for reassembly contexts. The user-level
`net_tcp_api` and `net_udp_api` cases cover repeated connect, close, duplicate
and process-exit cycles.

VirtIO completion validation was reviewed again with the entropy device: the
shared transport checks the descriptor id, the published head and the used
index before any callback, every driver resets before releasing ring or
buffer memory, and a reset that times out pins memory instead of freeing it.

### N09 validation and limits

Passing these tests is bounded evidence. The fuzzer covers parsers and
validators, not the state machines; protocol state is exercised by injected
segments in `net_tcp`, `net_tcp_transfer`, `net_path` and `net_pressure`.
There is no reseeding after boot, no per-connection sequence secret, no
SYN cookies, and the port range is 16384 ports drawn uniformly. Rate limits
are global, not per source. The exact commands, results and retained
artifacts are recorded in `docs/design/network-n07-n09-validation.md`.

## Configuration and DHCP (N10)

### The control device

`/dev/net` (`kernel/net/netdev.c`) is the only configuration interface.
Reading it returns a text snapshot taken on netd: the interface table with
its counters, one `link NAME MAC` line per Ethernet interface, the `inet`
line with the address, netmask and gateway, `arp` lines, and the IP, ARP,
ICMP, UDP, TCP and packet pool counters. `NETIOC_CONFIGURE` takes a
`struct net_config` and runs `net_configure`; an address of 0 removes the
address, netmask and gateway while the interface remains the broadcast
interface. `NETIOC_PING` is described under N11. MiniOS enforces no
permissions, so every process may reconfigure the network; the plan's
authority bound is the single-user model itself, not privilege separation.

`net(1)` wraps the device. `net apply`, a boot task of init after the
filesystems are mounted, reads `/etc/network`: `iface NAME static ADDRESS
NETMASK [GATEWAY]` configures once and `nameserver ADDRESS` lines are
written to `/etc/resolv.conf`; an `iface NAME dhcp` line is left to the
`dhcp` service of `/etc/init.conf`, `dhcpc -a`, which reads the same file
and is supervised by init (`init.md`). The default file asks for DHCP on
`eth0`. A missing file, a missing interface or an absent server never
delays the shell or the desktop: the task exits, the client exits with
status 0 when there is nothing to do, or keeps retrying with an
unconfigured interface. Booting without a NIC is unchanged.

### Broadcast before an address

DHCP needs traffic before the interface has an address. The kernel adds
exactly the limited-broadcast case: a route for 255.255.255.255 to the
attached Ethernet interface even while its address is 0; a UDP send to it
with source 0.0.0.0 when the interface is unnumbered, allowed only for a
socket with `SO_BROADCAST` (`EACCES` otherwise); frames to the broadcast MAC
reach IPv4 input, which accepts destination 255.255.255.255 for UDP only,
never answers it with ICMP and never hands it to TCP. Directed broadcast,
multicast and receiving with a nonzero mismatched address remain rejected.
A wildcard UDP bind on port 68 receives the replies; the client sets the
DHCP broadcast flag so servers reply to 255.255.255.255 rather than to an
address the client does not yet have.

### The client

`dhcpc(1)` (`user/coreutils/dhcpc.c`) implements DISCOVER, OFFER, REQUEST
and ACK over the socket API, validates replies by BOOTREPLY, transaction id,
client MAC and the magic cookie, parses options with bounds, and requires a
subnet mask, server identifier and a lease of at least ten seconds; anything
else, including NAK-less garbage, is ignored. It configures the interface
through `/dev/net`, writes `/etc/resolv.conf` from option 6, renews at T1 by
unicast, rebinds at T2 by broadcast, and at expiry or on NAK removes the
address, empties the resolver configuration and starts discovery again.
Retries back off from 4 to 64 seconds. `-1` acquires once for scripts and
tests, `-f` stays in the foreground, `-s`/`-p` address a unicast test server,
and `-a` takes the interface from `/etc/network` for the init service.
N10 had no address-conflict detection and kept no lease across a restart;
N16 added both and is described below.

`net_dhcp` runs `netdhcptest`, which drives the real client against a
scripted server on loopback: absence with bounded give-up, malformed and
unrelated replies before a valid lease, application through `/dev/net` and
`/etc/resolv.conf`, renewal at T1 with `ciaddr`, NAK removing the address,
rediscovery, and expiry with a silent server. It then obtains a live lease
from QEMU's user-mode DHCP server over the VirtIO NIC (10.0.2.15 from
10.0.2.2 with 10.0.2.3 as name server).

## Name resolution and tools (N11)

### Resolver

`libc/include/arpa/inet.h` gains `inet_aton`, `inet_addr`, `inet_ntoa`,
`inet_pton` and `inet_ntop` for AF_INET; other families fail with
`EAFNOSUPPORT`. `libc/include/netdb.h` provides `getaddrinfo`,
`freeaddrinfo` and `gai_strerror` for IPv4 with numeric services. Lookups
resolve numeric text and `localhost` without any file, then `/etc/hosts`,
then DNS over UDP to the `nameserver` entries of `/etc/resolv.conf` (three
at most, two attempts each, three seconds per attempt). Transaction ids come
from `/dev/urandom`, which the kernel random provider backs. An answer
counts only if its id matches, it came from the queried server (the socket
is connected), it is a response and it echoes the question. Names are
decoded with bounds, at most 16 compression pointers, each pointing
backwards, so loops and forward references fail with `EAI_FAIL`. A truncated
UDP answer is retried over TCP. CNAMEs are followed for at most eight
steps, with the target's address taken from the same response when present.
`RESOLV_CONF` and `HOSTS_FILE` in the environment redirect the files for
tests. N11 supported neither AF_INET6, service names, `AI_CANONNAME` output,
`getnameinfo` and reverse lookups, nor search domains and caching; N15 added
the last two and is described below.

`net_dns` runs `netdnstest`, a scripted server on loopback whose queried
name selects the behaviour: positive, NXDOMAIN, SERVFAIL, a compression
loop, truncation with TCP fallback, a CNAME with its target, an unbounded
CNAME chain, an unrelated transaction before the real answer, a silent
server, plus numeric, `localhost` and hosts-file lookups without a server.

### Tools

`ping(1)` uses `NETIOC_PING`: the kernel builds one echo request per call,
records it in one of four slots keyed by an identifier in the 0x4d00 range,
and wakes the caller on the matching reply or on an ICMP error quoting the
request, with a caller-bounded timeout of at most 60 seconds. There are no
raw sockets. `nc(1)` relays standard input and output over TCP or UDP as a
client or a one-connection server. `http(1)` performs one HTTP/1.0 GET over
plain `http://`; it makes no TLS claim and refuses `https://`. All three
resolve names through `getaddrinfo` and print the failure reason. `net_tools`
runs `nettools` with QEMU user networking: rejected and accepted static
configuration, the `/dev/net` snapshot, echo to the gateway and to an
unreachable host, HTTP against a loopback server (200, 404 and the
`https://` refusal), TCP and UDP relays and a refused connection.

The HTTP client of `http(1)` is `libc/src/net/http.c` behind
`minios/http.h`, which `pkg(1)` shares for signed repositories
(`packages.md`). It connects with a non blocking socket and waits in
`poll`, so a timeout bounds the connection and every wait for data;
`http -t SECONDS` sets it, and the default is 30 seconds. The body is
checked against `Content-Length` when the server sends one. A connection
closed before the announced length and bytes beyond it are errors that
name the server and the counts, and `http -o FILE` removes the file of a
failed transfer instead of leaving a truncated one. A caller may bound
the body, which `pkg` sets to the size its index gives. Redirects are
still not followed and a chunked body is not decoded. `pkg_repo`
exercises these paths against a host server that stops early and one
that stops answering.

## Release evidence (N12)

The complete affected set, the throughput baseline and the recorded
configuration are in `docs/design/network-n10-n12-validation.md`. The
network plan is marked complete there only for the milestones whose exit
criteria have evidence; the remaining limitations are listed per milestone.

## TCP window scaling and timestamps (N13)

N13 implements the two options of
[RFC 7323](https://www.rfc-editor.org/rfc/rfc7323.html). Every SYN the stack
sends offers both, and a connection uses an option only when the other SYN
of the handshake carried it too; a SYN ACK repeats only the options the
peer's SYN offered. `wire.c` reads the window scale option from SYN segments
only, since the RFC requires it to be ignored elsewhere, and the timestamp
option from every segment. A window scale or timestamp option with the
wrong length, or a second one in the same header, makes the segment
invalid, which is the policy N06 applied to a malformed MSS option.

### Stores and window scaling

The stores of a connection grew and moved out of the connection table.
When a connection is created, netd allocates a 65536-byte send store, a
131072-byte receive store and its 16384-byte presence bitmap from the
kernel heap with no lock held. A connection that cannot allocate them is
not created; passive open then counts a backlog drop and connect returns
`ENOBUFS`. The stores are released as soon as nothing can use them. The
receive store goes when the endpoint closes, because data that arrives
after the final close resets the connection instead of being stored, and
the send store goes when its last byte has been acknowledged after the
close. A connection that is finishing its FIN exchange or waits in
TIME_WAIT therefore holds its table entry and nothing more. With all 64
connections open the stores take 13 MiB.

The receive window is the free space of the receive store. Out-of-order
bytes lie inside that window and no longer shrink it, so reordering cannot
move the right edge to the left. The local shift is 2, the smallest that
expresses the whole store in the 16-bit field (`TCP_WINDOW_SHIFT`, checked
by a static assertion). With scaling in use the advertised window is
rounded down to a multiple of four bytes and input accepts exactly that
range; without scaling the window is capped at 65535 bytes of the larger
store. The peer's shift is clamped to 14, the limit of RFC 7323 section
2.3. The window of a SYN or SYN ACK is never scaled in either direction;
every later window of the peer is shifted by the peer's shift before it
limits transmission or takes part in the duplicate-ACK test.

### Timestamps

The timestamp clock is the network clock in milliseconds plus an offset
per connection that is drawn like an initial sequence number, so the values
do not reveal the uptime and the controlled clock drives them in tests.
Every segment of a connection that negotiated timestamps carries one and
echoes TS.Recent; a reset carries one as well, which RFC 7323 section 3.2
recommends. TS.Recent follows section 4.3: it takes the value of an
acceptable segment whose timestamp is not older than TS.Recent and whose
sequence starts at or before the last acknowledgement sent, which keeps the
timestamp of the earliest unacknowledged segment when segments arrive out
of order.

PAWS (section 5) runs before the sequence check. A segment without a
timestamp is dropped silently and counted in `timestamp_missing`. A segment
whose timestamp is older than TS.Recent is dropped, counted in
`paws_rejected` and answered with an ACK under the shared limit of 20
replies per second. A reset is exempt from both checks. After 24 days
without an update TS.Recent is no longer trusted, and the next segment
replaces it.

### Round-trip measurement

With timestamps, a round-trip sample is the timestamp clock minus the
echoed value of an ACK that acknowledges new data. One sample is taken per
flight; after a sample, the next one comes from the first ACK that covers
everything sent at the time of the previous one, which keeps the RFC 6298
gains of N07 meaningful. The echo identifies the transmission that the peer
acknowledged, so sampling continues after a retransmission; Karn's rule
applies only to connections without timestamps, which keep the timed
segment of N07. An echo of 0 and an echo more than 60 seconds old are
ignored.

The timestamp option takes 12 bytes of every segment. The MSS a peer
announces excludes options (RFC 6691), so a full segment of a connection
with timestamps carries the peer's MSS minus 12 bytes, and the congestion
window counts in that unit.

### Counters

`struct tcp_stats` and the `tcpopt` line of `/dev/net` report
`window_scaling` and `timestamps` (connections established with each
option), `paws_rejected`, `timestamp_missing` and `timestamp_samples`.

### N13 validation

`net_tcp_options` (kernel, controlled clock, fake capture interface) decodes
every captured segment with an option decoder of its own. It checks the
offer in a SYN (MSS 1460, shift 2, a timestamp with a zero echo, a 40-byte
header, window 65535) and the fallback when the SYN ACK carries MSS alone
(20-byte headers, full-MSS segments, window capped at 65535). On a
negotiated connection it checks that the ACK echoes the peer's timestamp and
advertises 32768 units, that one unit of the peer's shift 7 limits the
flight to 128 bytes, that full segments carry 1188 bytes for a peer MSS of
1200, and that the timestamp sample equals the 40 ms by which the controlled
clock moved. PAWS rejects an older timestamp with an ACK, a segment without
a timestamp is dropped silently, and a newer one is accepted and echoed.
TS.Recent stays unchanged for a segment beyond the last ACK and advances
when the hole is filled, a retransmission's echo yields a sample, and a
reset without a timestamp is accepted. A passive open repeats both options
and scales the window of the final ACK, a SYN without options is answered
with MSS alone, a shift of 15 is clamped to 14, malformed option lengths are
rejected and the 24-day rule restores acceptance. Every connection,
endpoint and packet buffer returns to the baseline.

`net_tcp_options_peer` runs the guest over the VirtIO NIC against the
scripted mode of `netpeer` (`tools/netpeer/scripted.c`) on the dgram link.
The peer builds and checks its headers without the guest's code. It answers
the SYN with MSS 1400, shift 5 and timestamps, receives 100000 bytes while
advertising 1024 units and acknowledging after four segments or 20 ms of
silence, sends one segment with an old timestamp, and then sends 3000 bytes
and a FIN with fresh ones. Its log records the guest's offer, the exact
stream, segments that end beyond the edge an unscaled window would give,
the guest's advertised window of 131072 bytes, a PAWS drop answered by an
ACK that does not cover the old segment, the acknowledged FIN, and neither
a missing timestamp nor a foreign echo. `check_capture.py` applies its own
rules to every capture. A connection whose SYNs both carried timestamps
has one on every later segment and each echo repeats a value the other
side sent, a connection without them has none, and no data segment ends
beyond the scaled right edge. `net_tcp_bulk` and `net_tcp_peer` require the
fallback case, because QEMU's user-mode stack answers with MSS alone.

## Selective acknowledgements and delayed ACKs (N14)

N14 adds the SACK option of [RFC 2018](https://www.rfc-editor.org/rfc/rfc2018.html),
the loss recovery of [RFC 6675](https://www.rfc-editor.org/rfc/rfc6675.html)
and delayed acknowledgements. Every SYN offers SACK-permitted next to the
options of N13, and a connection uses SACK only when the other SYN carried
it too. When both SACK-permitted and a timestamp are present, SACK-permitted
takes the place of the two padding bytes before the timestamp, as in the
layout of RFC 7323 appendix A. `wire.c` reads SACK-permitted from SYN
segments only and SACK blocks from every other segment; a block list whose
length is not 2 plus a multiple of 8, that holds no block or more than
four, or that repeats the option makes the segment invalid.

### Reporting received data

The receiver keeps up to four blocks to report (`TCP_SACK_REPORT`). When an
out-of-order segment is stored, the run of stored bytes that contains it is
found in the presence bitmap of the receive store, since RFC 2018 section 4
requires the first block to be that whole run, and it becomes the first
block; the blocks reported before follow in the order they were reported,
and a block the new run overlaps or touches is absorbed into it. Blocks at
or below `rcv_nxt` are dropped as the stream advances, and the list is
emptied when no hole remains. Every ACK of a SACK connection carries as
many blocks as fit, which is four without timestamps and three beside the
timestamp, and a data segment carries only as many as fit within the
peer's MSS together with the data. A duplicate report (D-SACK) is not sent.

### The scoreboard and loss recovery

The sender keeps a scoreboard of at most eight SACKed ranges
(`TCP_SCOREBOARD`), sorted and disjoint, within `[snd_una, snd_nxt]`. A
block that is empty, starts below `snd_una` or ends beyond `snd_nxt` is
ignored. A new block absorbs every range it overlaps or touches; when it
would need a ninth range the highest range is dropped and counted in
`scoreboard_drops`. Forgetting that the peer holds data is safe, because the
data is at worst sent again, and the ranges nearest `snd_una` decide what is
retransmitted next. A cumulative ACK trims the ranges it covers.

With SACK negotiated, an ACK that does not move `snd_una` counts as a
duplicate when it SACKs bytes not SACKed before (RFC 6675 section 2).
IsLost is decided per hole of the scoreboard, since every byte of one hole
has the same ranges above it, and the hole is lost when three ranges, or
more than two segments of SACKed bytes, lie above it. Recovery starts at the
third duplicate or when the first unacknowledged byte is already lost. It
records `snd_nxt` as the recovery point, sets the congestion window and the
threshold to half the flight (at least two segments) and retransmits the
first lost segment at once. From then on every ACK updates the scoreboard
and runs the transmission loop of section 5: while the window exceeds the
pipe (unSACKed bytes not lost plus retransmitted bytes, SetPipe of section
4) by a full segment, it sends the first lost byte above the highest
retransmission, then new data within the peer's window, then the first
unSACKed byte below SACKed data. At most 16 segments leave per ACK. The
congestion window does not grow during this recovery, which ends when
`snd_una` reaches the recovery point. The optional rescue retransmission of
rule 4 is not implemented.

A retransmission timeout on a SACK connection keeps the scoreboard, as
section 5.1 permits, and starts a timeout recovery. The window returns to
one segment with slow start, and every unSACKed byte below the recovery
point counts as lost, so the ACK-clocked loop retransmits the holes in
order and skips the SACKed ranges. A second consecutive timeout without
progress clears the scoreboard, because the peer may have discarded data it
had SACKed (RFC 2018 section 8). Connections without SACK keep the Tahoe
recovery of N07 unchanged.

### Delayed acknowledgements

A FIN, a segment that arrives out of order or repeats received data, and a
segment that fills part of a hole are acknowledged at once, as RFC 5681
section 4.2 asks. In-order data is acknowledged at once when the bytes
received since the last ACK reach two full segments (the receive MSS, less
12 bytes with timestamps); otherwise the ACK waits for `TCP_DELAYED_ACK_MS`,
100 ms, which is within the 500 ms limit of RFC 1122 section 4.2.3.2 and
RFC 9293 section 3.8.6.3. Any segment the connection sends acknowledges
everything received and cancels the pending delayed ACK, so a reply to a
request carries the ACK. The deadline is one more field that `tcp_schedule`
considers for the connection's timer.

A read that frees receive space announces the window on its own only when
the right edge moves by at least the smaller of half the store and one full
segment (receiver silly window avoidance, RFC 1122 section 4.2.3.3). Every
other segment carries the current window as before, and a peer's zero
window probe is a duplicate and is answered at once.

A FIN without data that arrives at `rcv_nxt` is accepted even when the
receive window is closed. RFC 9293 accepts no segment with length in a
closed window, but a FIN occupies no receive space, and dropping it made a
peer whose FIN met a full store wait for its own retransmission timeout
after the reader had emptied the store; the one-vCPU run of `net_tcp_bulk`
showed this as a stall of 1.1 seconds. `net_tcp_sack` checks the case.

### Counters

`struct tcp_stats` and the `tcpopt` line of `/dev/net` add `sack`
(connections that negotiated it), `sack_blocks_sent`,
`sack_blocks_received`, `scoreboard_drops`, `sack_recoveries`,
`sack_retransmits`, `delayed_acks` (ACKs that waited) and
`delayed_ack_timeouts` (ACKs sent by the timer).

### N14 validation

`net_tcp_sack` (kernel, controlled clock, fake capture interface) checks
the receiver with and without timestamps. An out-of-order segment is
acknowledged at once with its block, the most recent block comes first,
four blocks fit without timestamps and three beside them, filling a gap
reports the merged run, filling the first hole keeps the remaining blocks
in report order, and a contiguous stream carries no block. For the
delayed ACK, a 100-byte segment is not acknowledged until the timer fires
exactly 100 ms later, the second of two full segments is acknowledged at
once by one ACK for both, a reply carries the pending ACK, a 100-byte read
announces no window and a larger read does. On a flight of eight 1000-byte
segments whose first is lost, two duplicates do not start recovery, the
third retransmits exactly the lost segment and halves the window and
threshold to 4000, the pipe then holds further sending until more data is
SACKed, the next transmission is new data rather than SACKed data, only one
retransmission happens, and a cumulative ACK at the recovery point ends
recovery. After a timeout the ACK-clocked loop retransmits the hole below
the SACKed range and then the hole above it, and a second consecutive
timeout clears the scoreboard. Blocks below `snd_una`, beyond `snd_nxt` or
empty are ignored, nine separate blocks leave eight ranges with the highest
dropped, a covering block merges them, and a connection whose peer did not
permit SACK sends plain duplicate ACKs.

`net_tcp_options_peer` extends the scripted peer run of N13. The peer also
offers SACK-permitted, drops the first transmission of the guest segment
that reaches byte 50000 and answers every later segment with SACK blocks.
Its log shows how long after the original the guest retransmitted the
dropped segment, measured with the guest's own timestamps; the case
requires less than one second, the minimum retransmission timeout, and the
recorded run took 1 ms. The log also shows that no SACKed segment was sent
again, and the guest
reports one retransmission, made by SACK recovery, and no timeout. The peer
then sends three segments in the order 3, 1, 2 and finds the guest's
blocks correct, sends 100 bytes alone and receives the delayed ACK after
100 ms, and sends two full segments back to back and receives exactly one
ACK covering both. `check_capture.py` now also requires that SACK blocks
appear only on connections that negotiated SACK and never beyond the data
the other side has sent, and the case requires blocks in both directions.

## Resolver cache and search domains (N15)

N15 adds a cache of DNS answers, negative caching and the search list to
the resolver in `libc/src/net/resolv.c`. The cache lives in each process,
because libc is linked into every program and has no daemon to share
answers with. A long-running program that resolves names repeatedly gains
from it, while a short tool like `ping` exits before it could. A shared
cache would need a service and a protocol of its own and was not built.

### Cache

The cache holds 32 names (`CACHE_ENTRIES`). A key is the queried name in
lower case without a trailing dot, so `Host.Test` and `host.test.` share
an entry. When the cache is full, the entry used least recently is
replaced; expired entries are removed when they are found. A mutex
protects the table, so threads of one process may resolve at the same
time. Only DNS answers are cached; numeric names, `localhost` and
`/etc/hosts` never reach the cache.

A positive entry lives for the smallest TTL of the records the answer used:
the A records and every CNAME followed, in one response or across the
queries of a chain. A TTL of 0 means that the answer may serve this lookup
only (RFC 1035 section 3.2.1) and nothing is stored, and a TTL with the
high bit set counts as 0 (RFC 2181 section 8). Every entry lives at most
one hour (`CACHE_TTL_MAX`), so a changed record is seen within an hour
whatever TTL a server announces.

A negative answer, NXDOMAIN or NODATA (no address and no alias for the
name), is cached as RFC 2308 section 5 describes, for the smaller of the
TTL of the SOA record in the authority section and the SOA MINIMUM field
and at most the same hour, which lies within the one to three hours that the
RFC calls a sensible maximum. A negative answer without an SOA record is
not cached, as the RFC requires, and a server failure, a timeout or a
malformed answer is never cached. `res_cache_remaining` and
`res_cache_flush`, declared in `netdb.h` as MiniOS extensions, report the
remaining lifetime of a name and empty the cache; the tests use them.

### Search list

`/etc/resolv.conf` may hold a `search` line with up to six domains of at
most 256 bytes together, or a `domain` line with one; the last such line
wins, as in resolv.conf(5). `options ndots:N` (default 1, at most 15) sets
how many dots a name needs to be tried as given before the search list. A
name with a trailing dot is absolute and tried alone; a name with at least
`ndots` dots is tried as given and then with each domain; a name with fewer
dots is tried with each domain and then as given. Only a negative answer
moves on to the next candidate, and every candidate is looked up through
the cache, so a negative answer for one candidate is remembered as well.
`dhcpc` writes the domain name of option 15 as a `search` line after
checking that it holds only letters, digits, hyphens and dots, and
`net apply` copies `search` lines from `/etc/network`. The manual page
resolv.conf(5) documents the file.

### N15 validation

`net_dns_cache` runs `netdnstest cache` against the scripted loopback
server of N11, which now counts the queries it receives for every name. It
checks that a second lookup, in other case, costs no query; that an entry
with a TTL of 2 seconds expires and is queried again; that TTL 0 is not
cached; that a TTL of one day and a negative TTL of one day are capped at
one hour; that a CNAME with TTL 1 limits its chain to one second; that
NXDOMAIN with an SOA record of TTL 5 and MINIMUM 2 is cached for 2 seconds
and then queried again; that NXDOMAIN without SOA and SERVFAIL are not
cached; that NODATA with SOA is cached; and that 40 names leave the 32
most recently used in the cache. With `search example.test other.test` it
checks that a short name is tried with the first domain, that a negative
answer moves on to the second, that a trailing dot and a name with enough
dots are tried as given first, that `options ndots:3` puts the search list
first, and that a `domain` line acts as a list of one. `net_dns` still
passes unchanged, `net_dhcp` checks the `search` line written from option
15, and `net_tools` checks that `net apply` writes it from `/etc/network`.

## Address conflict detection and lease persistence (N16)

### ARP probes in the kernel

The DHCP client cannot send ARP itself, since there are no raw sockets, so
the kernel offers one narrow operation on `/dev/net`. `NETIOC_ARP_PROBE`
with a `struct net_arp_probe` (`minios/abi.h`) sends one ARP probe or one
announcement for an address on an Ethernet interface and then waits up to
the given time, at most ten seconds, for another host to claim the
address. A probe is a broadcast request with sender address 0 and a zero
target hardware address; an announcement carries the address as sender
and target (RFC 5227 sections 2.1.1 and 2.3). `arp.c` keeps two probe
slots under `arp_probe_lock` (`docs/design/locking.md`); a caller that
finds both in use gets `EBUSY`, an interface other than Ethernet
`EOPNOTSUPP` and an address that is not unicast `EINVAL`.

`arp_input` checks the slots before the checks that need a configured
address, because probing happens before configuration. While a slot is in
use, any ARP request or reply whose sender is the probed address, and any
probe for it, marks a conflict and wakes the caller, which gets
`EADDRINUSE` and the other host's hardware address. Packets from the
interface's own hardware address are ignored. The operation neither
changes the neighbour cache nor defends a configured address later
(RFC 5227 section 2.4). The `arp` line of `/dev/net` counts probes,
announcements and conflicts.

### Conflict detection in the client

After an ACK, and before it configures anything, `dhcpc` runs the probe
sequence of RFC 5227 section 2.1.1 with the constants of its section 1.1:
a random delay of up to one second, three probes one to two seconds apart
and a final wait of two seconds. A conflict is reported with the other
host's hardware address and answered with DHCPDECLINE, which names the
address (option 50) and the server (option 54), has ciaddr 0 and, as RFC
2131 table 5 requires, no parameter request list. The client then forgets
any saved lease, waits the ten seconds RFC 2131 section 3.1 asks for and
discovers again without backoff. Without a conflict the address is
configured and announced twice, two seconds apart. A kernel or interface
without the probe operation is reported once and treated as free, so
conflict detection never prevents configuration. `-A` divides every one of
these intervals by ten; the scripted tests use it.

### Lease persistence

The root image is rebuilt by every build and only the data volume mounted
at `/home` survives (`docs/design/storage.md`), and `fsinit` mounts it
before init starts the `dhcp` service. The lease therefore lives in
`/home/.local/state/dhcpc/IF.lease`. The lease is state rather than
configuration, so it lies where the XDG convention puts state, and the
client creates the file with its directories on the first lease. `-l FILE` selects another file. The file holds the
address, the server and the expiry in seconds of the real-time clock,
which the RTC sets at boot, so an expiry survives a reboot. It is written
under a temporary name and renamed at every bind and renewal, and removed
on a NAK, at expiry, and when an address is declined. Without a data
volume the file lands on the root image's `/home` and lasts until the next
build.

A client that starts with an unexpired saved lease enters INIT-REBOOT
(RFC 2131 sections 3.2 and 4.3.2) and broadcasts a REQUEST for the saved
address with ciaddr 0 and no server identifier, twice at most. An ACK for
that address goes through conflict detection like any other; a NAK or an
ACK for another address forgets the file and discovery starts; without an
answer, discovery starts with the saved address as the requested address
of the DISCOVER, which RFC 2131 permits. An expired file is ignored.

### N16 validation

`net_arp_probe` (kernel, injected ARP on a fake Ethernet interface without
an address) checks the probe and announcement frames field by field, that
an unrelated reply and the interface's own probe are no conflict, that a
reply from the address, a request from it and another host's probe for it
are conflicts reported with that host's hardware address, that a conflict
marks only the slot of its address, that a third slot is refused with
`EBUSY`, and the counters and argument errors. `net_arp` passes unchanged.

`net_dhcp` extends `netdhcptest`. The probes go out on the VirtIO NIC while
the scripted server answers on loopback, and QEMU's user-mode network
answers ARP for its gateway 10.0.2.2, so offering that address produces a
real conflict. The client reports it with QEMU's hardware address
52:55:0a:00:02:02, the server receives a DHCPDECLINE with the address, the
server identifier and no request list, and the next discovery binds
10.0.2.15 and saves it. The next run requests 10.0.2.15 in INIT-REBOOT
without a server identifier and is acknowledged; the run after that
receives a NAK, removes the file and discovers without asking for the old
address; an expired file leads straight to discovery. The live part now
also checks the saved file on `/home` and a second start that QEMU's server
acknowledges in INIT-REBOOT. The case's post script requires ARP probes
and announcements in the capture, which `check_capture.py` recognises by
their sender and target addresses.

## Release evidence (N13–N16)

The runs on one and four vCPUs, the fuzzing results, the measurements and
the limitations that remain are recorded in
`docs/design/network-n13-n16-validation.md`.
