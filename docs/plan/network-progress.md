# MiniOS TCP/IP progress record

This is section 7 of the network plan (`network.md`), stored in its own file because of its length.

## 7. Progress record

For each milestone, record its status as pending, in progress or complete;
implementation commits; tests and configurations; results; remaining
limitations; and any change to downstream dependencies. Preserve this plan's
distinction between restricted development milestones and the final supported
feature set.

### N00 (complete 2026-09-10)

Contracts and the feature matrix are in `docs/design/network.md`. The
controlled clock (`kernel/net/clock.c`) is the kernel seam; packet injection
arrives with the loopback interface in N02. The boot harness gained the
`nic` and `peer` case files, frame capture, peer readiness synchronization
and peer cleanup on every exit path; `tools/netpeer` is the host peer;
`tools/run.sh --nic` configures the interactive launcher. Tests: `net_clock`
(kernel, the clock), `net_harness` (QEMU 11.0 on macOS, dgram backend, real
kernel with a virtio-net device and no driver), `boot` (no NIC, unchanged),
and `make check-net` (host self test of the harness with a fake QEMU: success,
timeout and bad backend paths, no leftover process or port). Limitation: the
dgram backend has been verified on this host only.

### N01 (complete 2026-09-10)

The common socket layer (`kernel/ipc/socket.c`), the Unix backend behind
it (`kernel/ipc/unix_socket.c`), the Internet family with its protocol
registry (`kernel/net/inet_socket.c`), the Internet address types, message
flags, socket options and errors in the ABI and libc, the appended
`getsockname`, `getpeername`, `setsockopt` and `getsockopt` calls, flag
carrying `send`, `recv`, `sendto` and `recvfrom`, and lock-free stream I/O
on sockets (`FOPS_STREAM`). Tests: `sockets_api` (user), `net_socket`
(kernel, allocation baseline), regressions `sockets`, `evfd`, `fdflags`,
`pthreads`, `poll_wake`, `pipes`, `pipe_close`, `comp_core`, `comp_data`,
`comp_seat`, `comp_panel`, `gui_app`, `make check-headers`. Limitation:
`AF_INET` sockets validate and report `EPROTONOSUPPORT` until N05 and N06.

### N02 (complete 2026-09-10)

The packet pool with ownership checks and a control reserve
(`kernel/net/pbuf.c`), the RFC 1071 checksum and byte order primitives,
the interface table with counters and the loopback interface
(`netif.c`, `loopback.c`), the common IP entry point with a replaceable
handler (`net.c`), and the worker with bounded packet and request queues,
cancellation and deadline timers on the network clock (`worker.c`),
started from `kinit` before any interface exists. Limits: 256 buffers of
2048 bytes with 32 reserved, an input queue of 128, a request queue of 64,
batches of 32. Tests: `net_core` (every exit criterion of the milestone,
listed in `docs/design/network.md`), regressions `boot`, `net_clock`,
`net_socket`, `net_harness`, `sockets`, `sockets_api`, `smp`, `pipes`,
`sched`. Limitation: a request interrupted while queued is tested through
cancellation; the signal path uses the same code but no kernel test can
raise a signal against a kernel thread.

### N03 (complete 2026-09-11)

Implemented one modern VirtIO Ethernet device in `virtio_net.c`, with MAC-only
feature negotiation beyond VERSION_1, 32 dedicated RX slots, bounded TX slots,
worker-side delivery/refill/reclamation, and reset-before-free cleanup. Shared
VirtIO completions now validate full-width IDs, published heads and used-ring
advances before callbacks. Queue detachment is synchronized with IRQ traversal.

Tests: `net_virtqueue` exercises 66000 simulated completions, wraparound,
exhaustion, burst reclamation and malformed completions. `net_nic` exchanges
640 frames with the controlled raw peer and verifies reset accounting. Four
`net_nic_fail_*` cases verify partial initialization cleanup before and after
DRIVER_OK. Shared-driver regressions `blk`, `gpu_mode`, `input_tablet`,
`input_keyboard`, and `audio_pcm` pass. No-NIC `boot` remains supported.

Limitations: STATUS is not negotiated, so link state is administratively
assumed up. QEMU 11.0.3 supplies num_buffers=0 without merged receives; the
single-buffer compatibility exception is documented and tested. Reset timeout
pins memory safely; a hardware reset refusal was not fault-injected. Automatic
NIC restart/hotplug are not implemented.

### N04 (complete 2026-09-11)

Implemented Ethernet framing/dispatch, bounded ARP caches and pending queues,
static kernel configuration, connected/default routes and local delivery,
IPv4 validation with padding removal, ICMP echo and limited unreachable/error
handling. Routing selects the gateway before ARP. Protocol state and deadlines
belong to netd. Configuration is explicit and separate from socket creation.

Tests: `net_ipv4` covers gateway routing, ICMP echo, invalid headers, rejected
fragments/options, MTU errors, missing routes and ARP queue exhaustion/expiry.
`net_arp` checks asynchronous neighbor-timeout and link-down errors plus packet
reclamation. `net_icmp` exchanges echo requests in both directions with an
independent controlled Ethernet peer; the capture is checked for lengths and
checksums. Loopback protocol delivery is exercised by UDP and ICMP error tests.

Limitations: no incoming fragments, IP options, outgoing fragmentation,
forwarding, broadcast/multicast delivery, address-conflict detection or proxy
ARP. Static Ethernet configuration accepts ordinary subnets through /30.
These are restricted IPv4 host milestones, not general interoperability.

### N05 (complete 2026-09-11)

Implemented UDP endpoints, wildcard/specific binding and deterministic
ephemeral ports, unconnected and connected datagrams, source reporting,
checksums, empty datagrams, truncation/peek semantics, queue/byte limits,
nonblocking receive, poll and connected asynchronous errors. Teardown waits
for worker removal before releasing unread packets. Internet read/write use
kernel copy buffers, and recvmsg bounds copyout even when MSG_TRUNC returns
the full datagram length.

Tests: `net_udp` covers binding collisions, filtering, queue limits, checksum
validation/zero checksums, empty datagrams, truncation, ICMP errors and close
accounting. `net_udp_api` covers user address/iovec copies, guarded truncation,
flags/options, poll, concurrent receive/send, read/write, duplicate descriptors
and repeated port reuse. `net_udp_peer` completes 300 native host UDP echoes
(empty, odd-length and 1472-byte payloads); all 600 UDP packets in the capture
pass independent length/checksum validation. Existing Unix socket, descriptor,
eventfd, pthread and compositor IPC regressions pass.

Limitations: UDP payload limits are 1472 bytes over Ethernet and 1956 over
loopback; oversized sends return EMSGSIZE. Reuse options, AF_UNSPEC disconnect
and UDP shutdown are explicitly unsupported. Port unpredictability and
stronger resistance to forged ICMP errors remain N09 work. TCP connection
lifecycle is recorded below, with reliable transfer still reserved for N07.

The implementation is in the uncommitted working tree based on `87a10c3`;
unrelated pre-existing changes were preserved. Configuration, exact commands,
validation layers and artifact locations are recorded in
`docs/design/network-n03-n05-validation.md`.


### N06 (complete 2026-09-11)

Implemented TCP in `kernel/net/tcp/{core,input,output,socket,timer}.c`, with
separate bounded endpoint and connection tables, tuple lookup, port reservation,
active/passive and simultaneous open, SYN retransmission, independent half-open
and accept queues, blocking connect/accept, EINPROGRESS, poll and SO_ERROR.
Header/options/checksum validation and MSS negotiation use the existing IPv4
and worker contracts. Sequence and port providers are injectable for tests.

FIN/RST processing includes EOF after queued data, read/write half-close,
simultaneous close, retransmitted FINs, bounded orphan teardown and TIME_WAIT.
Final descriptor close removes the endpoint through a worker lifetime barrier.
Remaining protocol state has no socket pointer. Listener close releases both
queues and leaves accepted connections usable. Duplicated descriptors, signal
interruption and process exit use the shared socket/file lifetime rules.

Tests: `net_tcp` drives production parsing and output with controlled segments
and deadlines. `net_tcp_timer` verifies real worker dispatch at TIME_WAIT expiry.
`net_tcp_api` checks application copies, blocking/nonblocking calls, poll/errors,
short accept addresses, peek, half-close, signal interruption, duplication and
process exit. `net_tcp_peer` completes 20 native host connections through VirtIO
and QEMU user networking, with independent validation of captured TCP checksums.
The affected 21-case network/socket regression passes on both one and four TCG
vCPUs. Native TCP peer cleanup is tested after success and timeout.

Limits: 64 endpoints and connections, 4096 receive bytes per connection, at most
8 half-open and 16 completed children per listener. SYN and FIN allow three
retransmissions, orphan teardown is bounded to 30 seconds, and TIME_WAIT lasts
120 seconds. N06 allows one outgoing segment at a time for small in-order
exchanges. Unacknowledged data fails after 10 seconds. Reliable data recovery,
reordering and congestion control remain N07, and unpredictable sequence/port
selection remains N09. This milestone does not claim reliable bulk transfer.

Implementation remains uncommitted. Detailed contracts, commands, validation
layers and artifacts are recorded in `docs/design/network.md` and
`docs/design/network-n06-validation.md`.

### N07 (complete 2026-09-12)

Implemented reliable transfer in `kernel/net/tcp/transfer.c` and
`receive.c`: an 8192-byte send buffer retained until cumulative ACK, partial
writes, partial ACKs across wraparound, at most eight segments per flush
bounded by the smaller of the congestion and peer windows, an out-of-order
store sharing the 4096-byte receive ring with a presence bitmap and
first-arrival-wins overlap, FIN retained behind a hole, receive windows
that subtract out-of-order bytes, RFC 6298 RTT estimation with Karn's rule,
exponential backoff, Tahoe slow start, congestion avoidance and fast
retransmit with a recovery point, zero-window probes at `snd_una - 1`,
immediate ACKs and immediate small writes, and an explicit failure policy of
eight consecutive timeouts or 120 seconds without progress. Poll readiness
follows send-buffer room; blocked senders wake on freed space.

Tests: `net_tcp_transfer` (controlled segments and clock: partial write and
ACK, RTT and slow start, fast retransmit, timeout backoff, zero-window probe
and recovery, reorder, overlap and FIN before a gap), `net_tcp_bulk`
(262144 bytes each way with a native host peer through VirtIO and QEMU user
networking, byte-exact, capture checked), `net_pressure` (stalled connections
do not block unrelated ones). Regressions `net_tcp`, `net_tcp_timer`,
`net_tcp_api`, `net_tcp_peer` and the 21-case network/socket set pass on one
and four TCG vCPUs. Limitations: no window scaling, SACK, timestamps or
delayed ACKs; the receive window is at most 4096 bytes, which bounds
throughput on long paths; loss with a scripted host peer is not in the suite.

### N08 (complete 2026-09-12)

Implemented reassembly in `kernel/net/fragment.c`: eight contexts keyed by
interface, addresses, identification and protocol, 64 fragments and one
buffer each, a 30-second non-renewed deadline expired by a worker timer,
byte-granular overlap detection, exact duplicates ignored, conflicting
overlaps and inconsistent ends poisoning the key until expiry, and the
reassembled packet re-entering the ordinary protocol dispatch. Outgoing UDP
fragmentation after neighbour resolution with all buffers reserved first;
UDP sends up to the buffer capacity, `EMSGSIZE` beyond, DF on everything
else. Path MTU in `path.c`: a 128-entry recent-transmission table validates
ICMP quotes field by field except TTL and checksum before UDP or TCP see the
error, fragmentation-needed lowers a 16-entry ten-minute cache with the RFC
1191 plateaus when no MTU is given, TCP shrinks its MSS and retransmits, and
a black hole falls back through 576/296/68-byte paths from the second data
timeout while the retry and progress bounds continue to run. Route replacement
and interface down flush the tables. Packet buffers grew from 2048 to 8192
bytes (256 buffers, 2 MiB) so one buffer can contain `IPV4_MAX_PACKET`.

Tests: `net_fragment` (reverse order, duplicate, overlap quarantine, global
bound, oversized offset, worker expiry, recovery, pool baseline), `net_path`
(forged quote rejected, matching quote lowers MSS and retransmits, cache
expiry, black-hole fallback and `ETIMEDOUT`, route replacement), `net_ipv4`,
`net_udp`, `net_udp_peer` regressions. Limitation: no fragmented exchange
with a native peer, because QEMU user networking does not forward fragments
deterministically on this host; DF is never set on UDP.

### N09 (complete 2026-09-12)

Implemented the kernel random provider (`kernel/lib/random.c`, `chacha.c`)
seeded once at boot from a VirtIO entropy device (`virtio_rng.c`) with
ChaCha20 fast key erasure under `random_lock`; a missing device, a failed
read or a constant seed leaves it unavailable with one warning and no
fallback. TCP initial sequences and TCP/UDP ephemeral ports draw from it and
fail with `EAGAIN` while it is not ready; the test generator seam is
netd-only. The boot harness and `tools/run.sh` attach `virtio-rng-pci`
backed by `/dev/urandom`; case files `no-rng` and `rng-zero` select the
failure configurations. Host fuzzing of `wire.c`, `checksum.c`,
`socket_validate.c` and `chacha.c` (with the RFC 8439 known answer) under
ASan/UBSan is `make check-net-fuzz`, three seeds of 30 seconds; the runner
prefers Homebrew LLVM and falls back to UBSan when Apple's ASan runtime does
not start. Reply rate limits (20 per second for resets, challenge ACKs, ICMP
errors and echoes) are counted, a rejected NIC frame logs once, and
interrupts only record completions.

Tests: `net_random`, `net_random_unavailable`, `net_random_zero`,
`net_pressure` (three cycles of endpoint exhaustion, 1000-SYN floods,
slow readers, data-buffer exhaustion with a SYN through the control reserve,
interface down, a full TIME_WAIT table and recovery to baseline after real
timer expiry, packet low-water 31 of 256), `net_fragment` pressure, the
user-level connect/close cycles of `net_tcp_api` and `net_udp_api`. Fuzz
results, allocation marks and artifacts are in
`docs/design/network-n07-n09-validation.md`. Limitations: bounded evidence,
not an audit; no reseeding after boot, no SYN cookies, global rather than
per-source rate limits. Implementation remains uncommitted on
`bleeding-edge-net`.

### N10 (complete 2026-09-12)

Implemented `/dev/net` (`kernel/net/netdev.c`: text snapshot, `NETIOC_CONFIGURE`,
`NETIOC_PING`) and `/dev/urandom`, the deliberate limited-broadcast path
(route to 255.255.255.255 on the attached interface, `SO_BROADCAST` UDP sends
from 0.0.0.0 while unnumbered, broadcast MAC frames accepted for UDP only, no
ICMP replies to broadcast), `net(1)` with `/etc/network` applied by init in the
background, and `dhcpc(1)` with validation, bounded backoff, renewal,
rebinding, NAK and expiry handling that removes the address and the resolver
configuration. Tests: `net_dhcp` (scripted loopback server: absence, malformed
and unrelated replies, lease application, T1 renewal, NAK, rediscovery, expiry;
then a live lease from QEMU's user-mode server), `net_tools` (static
configuration and snapshot). Limitations: no address-conflict detection, no
DHCP over a second interface, permission enforcement is absent by design.

### N11 (complete 2026-09-12)

Implemented `arpa/inet.h` conversions, `netdb.h` with an IPv4 `getaddrinfo`
subset (`lib/libc/src/net/resolv.c`: numeric, `/etc/hosts`, DNS over UDP with
bounded retries, id/server/question matching, defensive compression parsing,
CNAME limits and TCP fallback for truncation, random ids from `/dev/urandom`),
`ping(1)` through the narrow echo interface, `nc(1)`, `http(1)` (HTTP/1.0
GET, `http://` only, no TLS) and manual pages for all five tools. Tests:
`net_dns` (scripted server covering positive, negative, failure, loop,
truncation, CNAME, chain limit, unrelated replies, silent and absent servers,
numeric and hosts lookups), `net_tools` (ping success and failure, HTTP 200,
404 and `https://` refusal, TCP/UDP relays, refused connection). Limitations:
no IPv6, service names, search domains, caching or reverse lookups; the tools
are diagnostics, not a general client library.

### N12 (complete 2026-09-12)

The full affected set (all 26 network cases, the socket, descriptor, eventfd,
pthread, pipe, compositor, GUI and block regressions) passes on one and four
TCG vCPUs. The native bulk transfer provides the throughput baseline. Results,
QEMU version, commands and artifacts are in
`docs/design/network-n10-n12-validation.md`; the design record is
`docs/design/network.md`. Unresolved items are listed there per milestone
and remain documented limitations rather than open exit criteria: scripted
loss with a native peer, fragmented traffic with a native peer, address
conflict detection, and formatting verification without a repository style.

### N13 (complete 2026-09-30)

RFC 7323 window scaling and timestamps are implemented in `wire.c` and
`kernel/net/tcp/`. Every SYN offers both options, and a connection uses an
option only when the other SYN carried it. The peer's shift is clamped to
14 and the local shift is 2 over a 131072-byte receive store; the send
store contains 65536 bytes. The stores are allocated per connection and
released as soon as no endpoint can use them, and out-of-order bytes remain
inside the window instead of shrinking it. PAWS applies the 24-day rule,
TS.Recent follows section 4.3, one timestamp sample per flight feeds the
RFC 6298 estimator without Karn's restriction, and segments carry the
peer's MSS minus the 12-byte option. The `tcpopt` line of `/dev/net` shows
five new counters.

`net_tcp_options` covers the option rules with injected segments and the
controlled clock. `net_tcp_options_peer` runs the VirtIO NIC against the
new scripted mode of `netpeer`, which offers both options and reports what
it observed, and its capture is checked against the option rules that
`check_capture.py` now applies to every TCP capture. `net_tcp_bulk` and
`net_tcp_peer` require the fallback that QEMU's user-mode stack causes.
Two existing tests changed because the window grew. `net_tcp` moves its
out-of-window reset beyond 65535 bytes, and `net_pressure` fills the larger
store in several segments. The parser changes were fuzzed with structured
options. QEMU's user-mode stack never offers the options, so a native host
stack exercises only the fallback; the scripted peer provides the
negotiated case on the wire.

### N14 (complete 2026-09-30)

RFC 2018 SACK, RFC 6675 loss recovery and delayed ACKs are implemented in
`wire.c` and `kernel/net/tcp/`. SACK-permitted is offered in every SYN and
used only when both SYNs carried it. The receiver reports up to four blocks
with the run containing the newest out-of-order segment first, found in the
presence bitmap. The sender maintains a scoreboard of eight ranges and drops the
highest when a ninth is needed, counts duplicates as ACKs that SACK new
data, decides IsLost per hole, enters recovery at the third duplicate or an
earlier loss, halves the window, and runs the SetPipe and NextSeg loop with
rules 1 to 3 and at most 16 segments per ACK. A timeout retains the
scoreboard for an ACK-clocked recovery of the holes, and a second
consecutive timeout clears it. ACKs of in-order data wait 100 ms unless two
full segments are owed; FINs, duplicates, out-of-order data and hole fills
are acknowledged at once, and window updates after reads follow receiver
silly window avoidance. Eight counters are added to the `tcpopt` line.

`net_tcp_sack` covers these rules with injected segments and the controlled
clock. In `net_tcp_options_peer` the scripted peer now also drops one guest
segment and reports its repair from SACK information in less than one
second of guest time, without a resend of SACKed data or a timeout. It also
reports correct guest blocks for reordered data, a delayed ACK after 100 ms
and a single ACK for two full segments, and the capture checker verifies
SACK blocks in both directions. The existing `net_tcp_options` case now
expects the delayed ACK of an in-order segment. The optional rescue
retransmission and D-SACK are not implemented, and loss with a native host
stack is still not scripted, because QEMU's user-mode stack neither drops
on request nor offers SACK. A FIN without data at `rcv_nxt` is now
accepted in a closed window, which removed a stall of one retransmission
timeout that the one-vCPU run of `net_tcp_bulk` showed when QEMU's FIN met
a full receive store.

### N15 (complete 2026-09-30)

The libc resolver caches DNS answers per process in 32 entries with
least-recently-used replacement, for the smallest TTL of the records used
and at most one hour; TTL 0 is not stored. Negative answers are cached for
the smaller of the SOA TTL and the SOA MINIMUM, at most one hour, and only
when the answer carries an SOA record; server failures and timeouts are
never cached. `search`, `domain` and `options ndots:N` in
`/etc/resolv.conf` select the candidate names in the order of
resolv.conf(5). `dhcpc` writes the domain name of option 15 as the search
list and `net apply` copies `search` lines from `/etc/network`.
`res_cache_remaining` and `res_cache_flush` are MiniOS extensions of
`netdb.h`.

`net_dns_cache` covers the cache, the negative cache and the search list
against the scripted server, which now counts queries; `net_dns`,
`net_dhcp` and `net_tools` pass with their new checks for the search list.
The `net_tools` case now also runs `net apply` and restores the files it
changed. The cache is not shared between processes, so a short-lived tool
queries the server every time; a shared cache would need a service of its
own.

### N16 (complete 2026-09-30)

`NETIOC_ARP_PROBE` on `/dev/net` sends one RFC 5227 probe or announcement
and waits up to ten seconds for a conflict, which `arp_input` detects
from the sender or the probed target of any ARP packet before it looks at
the interface's address. The new `arp_probe_lock` protects two probe
slots and was recorded in `docs/design/locking.md` first. `dhcpc` probes
an acknowledged address with the RFC 5227 timing, declines it on a
conflict, waits ten seconds and discovers again, and announces a
configured address twice. It saves the lease in
`/home/.local/state/dhcpc/IF.lease`, the only persistent location, and
starts in INIT-REBOOT when the saved lease has not expired. `-l` selects
the file and `-A` shortens the intervals for tests.

`net_arp_probe` covers the kernel rules with injected ARP packets.
`net_dhcp` now produces a real conflict through QEMU's ARP answer for its
gateway, checks the DHCPDECLINE, INIT-REBOOT with an ACK and with a NAK,
an expired file, the saved live lease and a live INIT-REBOOT, and its new
post script requires probes and announcements in the capture. Its existing
steps use their own lease files and `-A`, and one wait grew from one to
two seconds because conflict detection now precedes configuration.
Defence of a configured address (RFC 5227 section 2.4) and conflict
detection for static configuration are not implemented.
