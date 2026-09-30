# MiniOS TCP/IP implementation plan

N00–N12 are complete on the branch `bleeding-edge-net` (2026-09-12), and
N13 to N16 are complete on `bleeding-edge-net-options` (2026-09-30).
Section 7 holds the evidence and the remaining limitations of each
milestone, and `docs/design/network-n13-n16-validation.md` holds the
release evidence of N13 to N16.

Prepared on 2026-09-10 from source inspection of the working tree on
`bleeding-edge-pkg`, whose HEAD was `1a95395`. The working tree contained
uncommitted work. These findings describe that inspected source, not a tested
networking runtime. Recheck integration points before implementation.

This is a separate plan from `PLAN.md`. Milestone identifiers use the `N`
prefix so they do not renumber the existing development milestones. Current milestone status and validation evidence are recorded in section 7. Proposed paths, commands, and test names are
design targets unless explicitly described as existing.

## 1. Intended result and scope

Deliver a native IPv4 host stack for MiniOS with loopback, one modern VirtIO
Ethernet interface, ARP, ICMP, UDP, TCP, static and DHCP configuration, DNS
resolution, and small diagnostic and client/server programs. Applications use
MiniOS file descriptors, blocking and nonblocking sockets, and `poll()`.

The initial target is QEMU's existing x86_64 machine configuration. Use
controlled local peers throughout development. A successful ping or TCP echo
exchange is an intermediate result, not evidence that the complete stack is
reliable or interoperable.

Deferred features include IPv6, forwarding/router operation, guest NAT,
multicast membership, raw sockets for general applications, other NIC drivers,
multiple hardware queue pairs, hardware checksum/segmentation offloads, and
zero-copy application I/O. TLS and HTTPS are separate application/library
projects; TCP support does not provide either one.

This plan assumes a native implementation. Selecting and evaluating an
external protocol stack would be a separate design decision; this
investigation did not evaluate library versions, licenses, or porting effort.

## 2. Existing foundations and integration constraints

| Existing source | Finding | Required treatment |
|---|---|---|
| `kernel/drivers/virtio/virtio.c`, `kernel/include/drivers/virtio/virtio.h` | Modern PCI VirtIO transport, MSI-X, split queues, at most four queues per device and 128 descriptors per queue | Reuse for one RX queue and one TX queue; negotiate only supported features. |
| `kernel/ipc/socket.c` | Unix-domain streams, shared connection rings, descriptor passing, and a file-operations-pointer socket type check | Introduce common socket dispatch and keep Unix connection state in its backend. |
| `kernel/syscall/sys_ipc.c` | Socket creation accepts only `AF_UNIX`; the third argument is treated as flags; bind/connect assume Unix addresses; accept ignores address outputs | Add family-aware address handling and real Internet protocol selection without breaking existing Unix callers. |
| `libwire/src/client.c`, `libwire/src/server.c` | MiniOS callers pass socket flags in the third argument | Preserve this legacy Unix convention initially; use standard type flags for new Internet calls. |
| `libc/src/ipc.c` | `send()` and `recv()` call write/read and ignore message flags | Route through socket operations and explicitly validate flags. |
| `kernel/fs/file.c` | Read and write hold the same position mutex while the backend may block | Add an explicit position-independent I/O path for sockets so a blocked read does not exclude a concurrent write. |
| `kernel/ipc/poll.c` | Poll registers an object's source before checking readiness | Give each new socket a stable poll source across connection state changes. |
| `kernel/sched/wait.c`, `kernel/drivers/timer.c` | Timed waits and monotonic time already exist | Run protocol timer work in a worker thread; timer interrupts arrange wakeups only. |
| `kernel/arch/x86_64/boot.c` | `kinit()` initializes devices requiring a schedulable thread | Initialize networking here with explicit worker/device publication order. |
| `kernel/Makefile` | Source directories are enumerated in `SUBDIRS` | Add the proposed network source directories explicitly. |
| `tests/run_qemu_test.sh`, `tools/run.sh` | Boot tests have no network peer lifecycle; the interactive launcher accepts extra QEMU arguments | Add reproducible network configuration and peer setup/cleanup to the test harness. |

No native Internet address headers, network protocol implementation, resolver,
or kernel entropy facility was found in the searched source. Some networking
errno values already exist; add missing values consistently in kernel and libc.

## 3. Architecture and invariants

### Socket and protocol separation

Introduce a common socket object with family, type, protocol, backend
operations, error state, references, and a stable poll source. Keep descriptor
installation, close-on-exec, and file reference handling shared. Keep
`SCM_RIGHTS` processing specific to Unix sockets.

Internet endpoints use separate UDP and TCP state. A TCP connection may remain
alive after its application file has closed because retransmissions, FIN
processing, or TIME_WAIT still require state. File lifetime, connection
lifetime, and packet lifetime must be accounted for separately.

### Worker and locking model

Use one kernel networking worker initially. It owns protocol state transitions,
route/neighbor updates, and protocol deadlines. System calls exchange copied
data and bounded requests with the worker; short endpoint locks protect the
application-visible queues and readiness state. The worker never waits for a
peer response, application buffer space, or a device descriptor while holding
up unrelated connections.

Document field ownership and lock ordering in `docs/design/locking.md` before
introducing the locks. No spinlock or RCU read section may span a blocking
operation or a potentially faulting user copy. Do not introduce per-CPU
protocol tables or lock-free packet queues until measurements justify them.

Wakeups must be synchronized with condition checks. The worker sleeps until
work arrives or the earliest protocol deadline expires. Process packets and
requests in bounded batches, checking timers between batches, so continuous
traffic cannot starve retransmissions or other kernel work.

### Packet ownership

Start with bounded kernel-owned packet buffers containing data length,
capacity, header headroom, interface metadata, and ownership/reference state.
Use explicit byte-order helpers and bounds-checked access to wire headers.
Never retain application pointers for asynchronous work or DMA.

VirtIO callbacks run under the queue lock, before the transport releases the
completed descriptor chain. Callbacks record completions and wake the worker;
they do not run the protocol stack or block. Refill logic must respect that
descriptor-release order. Buffers published to the device remain valid until
completion or a completed device reset that ends DMA access.

NIC transmit completion permits reclaiming a device transmission buffer. TCP
acknowledgement permits reclaiming acknowledged stream data. These are
independent events. The first implementation may copy retransmission data into
fresh transmit buffers to keep the ownership rules simple.

### Resource policy

Set documented limits for packet storage, sockets, per-socket queued bytes,
pending requests, ARP entries and queued packets, fragments, half-open TCP
connections, accepted connections, and TIME_WAIT state. Define failure behavior
for every limit. Keep bounded capacity available for control traffic under
data-buffer pressure so ACKs and teardown do not depend on unbounded allocation.

## 4. Milestone overview

| Milestone | Deliverable | Dependencies |
|---|---|---|
| N00 | Contracts, deterministic tests, and controlled peer harness | None |
| N01 | Generic socket dispatch and safe full-duplex I/O | N00 |
| N02 | Packet buffers, interfaces, loopback plumbing, worker and timers | N00 |
| N03 | Modern VirtIO NIC with bounded RX/TX ownership | N02 |
| N04 | Static IPv4, Ethernet, ARP, routing and ICMP | N02, N03 |
| N05 | UDP sockets and datagram semantics | N01, N04 |
| N06 | TCP establishment and connection lifecycle | N01, N04; execute after N05 |
| N07 | Reliable TCP transfer, flow and congestion control | N06 |
| N08 | IPv4 fragmentation/reassembly and path-MTU handling | N05, N07 |
| N09 | Entropy integration and robustness under hostile input/load | N05, N07, N08 |
| N10 | Persistent configuration and DHCP client | N05, N09 |
| N11 | DNS resolver and user-facing networking tools | N07, N08, N09, N10 |
| N12 | Interoperability, regressions, documentation and release evidence | N00–N11 |
| N13 | TCP window scaling and timestamps | N07, N09, N12 |
| N14 | TCP selective acknowledgements and delayed ACKs | N13 |
| N15 | Resolver cache, negative caching and search domains | N11 |
| N16 | DHCP address conflict detection and lease persistence | N10 |

N01 and N02 are architecturally independent after their shared contracts are
settled. The table does not authorize parallel implementation or agent work.

### N00. Contracts and test infrastructure

Scope:

- Write the socket dispatch, address-copy, packet ownership, worker request,
  clock, and error contracts before implementing their consumers.
- Fix the initial feature matrix: IPv4 host operation, one Ethernet NIC,
  loopback, software checksums, and no negotiated segmentation offloads.
- Check wire layouts and protocol requirements against primary specifications
  during implementation; record the specifications and supported subset in the
  design documentation. The source investigation was not a standards audit.
- Create host-test seams for packet injection and a controllable monotonic
  clock. Keep protocol logic independent of PCI and user pointers.
- Extend the boot harness with explicit NIC/backend configuration, controlled
  host peer startup, readiness synchronization, packet capture, and cleanup on
  success, failure, and timeout. Verify backend capabilities on the actual
  host; do not assume every QEMU backend supports identical packet behavior.
- Use an isolated link or packet harness for Ethernet/ARP/ICMP tests and a
  controlled host socket peer for application interoperability. Public Internet
  services must not be test dependencies.

Likely paths: `tests/run_qemu_test.sh`, `tools/run.sh`, `qemu.conf.example`,
top-level `Makefile`, proposed `tests/net/` and `docs/design/network.md`.

Exit criteria: the harness can start and stop a peer deterministically, collect
diagnostics, exercise a fake clock, and leave no peer processes or port
reservations after failure. A no-NIC boot retains its current behavior.

### N01. Socket dispatch, ABI and full-duplex I/O

Scope:

- Introduce common socket operations and retain the existing Unix backend.
- Preserve legacy third-argument flags for `AF_UNIX` initially. Define
  `AF_INET` creation with a protocol argument and flags in the type argument;
  reject unsupported combinations. Keep existing syscall numbers stable and
  append new calls where needed.
- Add Internet address types and bounded address input/output helpers. Implement
  accept peer-address reporting, address-length truncation conventions,
  `getsockname()`, `getpeername()`, and socket-option dispatch.
- Add message flags and socket errors required by the initial feature set.
  Reject unsupported flags rather than silently ignoring them. Route libc
  send/receive through that interface; provide sendto/recvfrom wrappers when
  the message interface supports addresses.
- Audit iovec accumulation for overflow, all user output ranges, and partial
  failure cleanup while restructuring message handling.
- Add position-independent file I/O for sockets. Preserve regular-file offset
  locking and do not globally remove file mutexes.
- Keep poll-source identity stable; specify readiness for disconnected,
  connecting, established, failed, and closed endpoints.

Likely paths: `kernel/ipc/socket.c`, `kernel/include/ipc/socket.h`,
`kernel/syscall/sys_ipc.c`, syscall declarations/table, `kernel/fs/file.c`,
`kernel/include/fs/vfs.h`, `kernel/include/minios/abi.h`, kernel/libc errno
headers, `libc/include/sys/socket.h`, `libc/src/ipc.c`, proposed Internet headers.

Exit criteria: existing `sockets`, `fdflags`, `evfd`, `pthreads`, and relevant
compositor IPC tests pass. A new test demonstrates a blocked reader and an
independent writer sharing one socket. Invalid addresses, flags and oversized
iovecs fail without leaks. Internet protocols may still report unsupported
until their milestones land.

### N02. Packet core, interfaces, worker and deadlines

Scope:

- Implement bounded packet pools and ownership checks, checksum/byte-order
  primitives, an interface abstraction, and interface counters.
- Add loopback delivery through the common IP input entry point, initially
  exercised with a test handler before IPv4 is implemented.
- Implement the networking worker, bounded producer queues, endpoint/request
  references, monotonic deadline scheduling, and cancellation.
- Define behavior for allocation failure, a full request queue, interface-down
  events, and requests interrupted while waiting for completion.
- Initialize the core before publishing interfaces or accepting requests.

Proposed paths: `kernel/net/{packet,checksum,netif,loopback,worker,timer}.c`,
`kernel/include/net/`, `kernel/Makefile`, and `kernel/arch/x86_64/boot.c`.

Exit criteria: deterministic tests cover allocation failure, ownership
transitions, odd-length checksums, queue exhaustion, timer cancellation,
deadline arrival during waiter registration, and work arriving just before
sleep. Repeated cycles restore allocation counts to baseline, and sustained
packet input does not indefinitely postpone timers.

### N03. VirtIO Ethernet driver

Scope:

- Discover a supported modern VirtIO network device and negotiate only the
  implemented feature set. Use one receive and one transmit queue.
- Validate configuration availability and obtain the MAC address. Define a
  documented initial link-status policy and handle relevant configuration
  changes when the corresponding feature is negotiated.
- Prepare receive buffers before enabling normal traffic. Validate descriptor
  identifiers, used lengths, VirtIO headers, and packet capacity before access.
- Defer protocol processing and refill to the worker. Handle descriptor
  exhaustion without losing buffers or requiring a spare descriptor inside a
  completion callback.
- Keep transmit memory alive through completion and implement safe cleanup for
  partial initialization, queue failure, and device reset.
- Audit any shared transport changes against block, input, GPU, and sound users.

Proposed paths: `kernel/drivers/virtio/virtio_net.c`, its header, network
interface integration, shared VirtIO transport only where necessary, launcher
and test harness configuration.

Exit criteria: capture verifies repeated raw frame RX/TX on a controlled link;
ring wraparound, bursts, exhaustion, malformed completions and reset paths
preserve accounting. Run the affected shared-driver regressions after any
transport change. Booting without a NIC remains supported.

### N04. Ethernet, ARP, static IPv4 and ICMP

Scope:

- Add Ethernet dispatch and bounded ARP caches with expiry, retry limits and
  per-neighbor/global pending-packet limits. Failure to resolve a neighbor
  must complete or fail waiting work rather than retaining it indefinitely.
- Add static interface address/netmask configuration, loopback addressing,
  connected routes and a default route. Choose route and next hop before ARP;
  remote destinations resolve the gateway's MAC address.
- Validate IPv4 version, header length, total length, checksum, destination and
  supported options. Trim Ethernet padding to the IP length.
- Implement ICMP echo and the initial error handling/generation needed by the
  stack, including rate limits and suppression of inappropriate error replies.
- Provide a minimal kernel configuration/control interface for tests and later
  user tools. Keep ordinary socket creation separate from interface mutation.
- Explicitly reject incoming fragments during this milestone and constrain
  outgoing packets to the interface MTU. Count these restrictions; do not
  describe this stage as general IPv4 interoperability.

Proposed paths: `kernel/net/{ethernet,arp,route,ipv4,icmp,control}.c`, matching
headers and boot tests `net_ipv4`, `net_arp`, `net_icmp`.

Exit criteria: loopback and a controlled Ethernet peer exchange ICMP messages;
off-subnet routing uses the configured gateway; invalid headers, unreachable
neighbors and missing routes produce bounded, observable outcomes.

### N05. UDP sockets

Scope:

- Implement `AF_INET` datagram sockets, wildcard/specific binding, collision
  rules, ephemeral-port allocation, and local delivery/demultiplexing.
- Support unconnected sendto/recvfrom and connected UDP peer selection/filtering.
  Preserve source addresses and datagram boundaries through sendmsg/recvmsg.
- Implement UDP length and pseudo-header checksum validation with the defined
  IPv4 zero-checksum behavior. Preserve zero-length datagrams.
- Specify atomic send behavior, oversized datagram errors, receive truncation,
  message flags, and the difference between a zero-length read request and
  receiving an empty datagram.
- Add nonblocking behavior, poll readiness, queue limits, and a documented
  policy for mapping relevant ICMP errors to connected sockets.

Proposed paths: `kernel/net/{inet_socket,udp}.c`, libc Internet helpers and
tests `net_udp`, `net_udp_api`.

Exit criteria: host/guest and loopback tests cover empty datagrams, multiple
queued datagrams, truncation, address reporting, port collisions, wildcard
bindings, invalid checksums, full queues, concurrent operations and close.
Unsupported oversized traffic fails explicitly while N08 is pending.

### N06. TCP establishment and connection lifecycle

Scope:

- Implement TCP endpoint lookup, port allocation and a connection control
  block with sequence-space fields and explicit state transitions.
- Add active/passive open, separate incomplete-handshake and accept queues,
  bounded SYN retransmission, refused connections, and handshake timeouts.
- Implement blocking connect/accept and nonblocking connect with `EINPROGRESS`.
  Completion wakes poll; applications obtain success or failure through
  `SO_ERROR`, not by treating writability alone as success.
- Parse TCP headers/options safely and establish the initial MSS policy.
- Implement FIN/RST validation, read EOF after queued data, half-close,
  simultaneous close, teardown deadlines, and TIME_WAIT lifetime rules.
- Ensure close, signals, descriptor duplication and process exit cannot leave
  worker requests referring to freed sockets. Closing a descriptor starts
  asynchronous protocol teardown where required.
- Use injectable sequence/port generation in deterministic tests. Until N09
  supplies an entropy-backed provider, this is a controlled-development stack.

Proposed paths: `kernel/net/tcp/{core,input,output,socket,timer}.c` and tests
`net_tcp_connect`, `net_tcp_close`. Add nested source directories to the build.

Exit criteria: controlled peer tests cover active/passive open, duplicate and
lost handshake packets, backlog exhaustion, connect failure, poll completion,
half-close, resets, simultaneous close and TIME_WAIT expiry. This stage does
not claim reliable bulk data transfer.

### N07. TCP reliable transfer and congestion control

Scope:

- Add bounded send buffering, segmentation and storage retained until ACKed.
  Handle partial ACKs and sequence-number wraparound with dedicated helpers.
- Add ordered receive delivery and bounded out-of-order storage. Handle
  duplicate/overlapping segments consistently without duplicating stream bytes.
- Advertise receive windows from actual capacity and send window updates when
  readers free space. Account for out-of-order storage in that capacity.
- Implement retransmission scheduling, exponential backoff, RTT measurement
  with retransmission ambiguity handling, and an explicit retry/failure policy.
- Implement an initial established congestion-control algorithm with slow
  start, congestion avoidance and a defined loss-recovery policy. Keep the
  congestion window distinct from the peer's advertised receive window.
- Implement zero-window probes/recovery and small-write/ACK policies that cannot
  leave mutually waiting endpoints stalled indefinitely.
- Preserve independent read/write progress, partial writes, SIGPIPE/message
  flag behavior, errors, and poll readiness as queues and states change.

Exit criteria: scripted packet loss, duplication, reordering, delayed ACKs,
partial ACKs, zero windows and sequence wraparound preserve exact stream data
and eventually complete or return a documented error. Transfers exceed socket
buffer sizes in both directions. A stalled connection cannot block unrelated
connections. Fake-clock tests verify retransmission and congestion-state
changes; host peers verify interoperability.

### N08. IPv4 reassembly and path MTU

Scope:

- Add bounded fragment reassembly with complete keying, offset/length checks,
  expiry, per-datagram/global limits, and a documented overlap rejection policy.
- Complete the outgoing IPv4 fragmentation/DF policy for UDP and report
  datagram-size failures consistently through the socket API.
- Validate and associate incoming ICMP errors with the affected flow before
  changing its state. Add path-MTU state with expiry and update TCP segment
  sizing for smaller paths.
- Define recovery for paths where useful ICMP feedback is absent, using bounded
  fallback/retry behavior. Route or interface changes invalidate stale state.
- Ensure reconstructed packets re-enter the same transport validation path as
  unfragmented packets.

Exit criteria: tests cover out-of-order, duplicate, overlapping, missing and
oversized fragments, expiry, memory exhaustion, reduced-MTU paths, forged or
unrelated ICMP errors and missing ICMP feedback. Valid traffic resumes after
resource pressure. Remove the N04/N05 limitations only when their replacement
behavior is tested and documented.

### N09. Entropy, input robustness and resource recovery

Scope:

- Implement an entropy-backed kernel random provider suitable for initial TCP
  sequence generation and ephemeral-port selection. Select an available source,
  for example a separately implemented VirtIO entropy device, after checking
  platform support; specify seeding, failure and unavailable-source behavior.
  Do not silently substitute libc rand or a timer-derived value.
- Keep deterministic test injection separate from normal operation and expose
  whether the required provider is ready before broader network exposure.
- Fuzz packet parsers and socket address/iovec handling with host sanitizers
  where available. Cover lengths, options, checksums and integer overflow.
- Exercise SYN pressure, fragment pressure, ARP churn, slow readers, stalled
  writers, excessive TIME_WAIT state, interface-down events and repeated
  connect/close/process-exit cycles.
- Verify control traffic can progress under data pressure, errors are rate
  limited, and packet floods do not create unbounded interrupt work or logs.
- Review shared VirtIO completion validation and teardown for failures that
  could leave live DMA targeting freed memory.

Exit criteria: record fuzz inputs/seeds and duration, allocation high-water
marks, post-test recovery, and any unresolved failures. Repeated stress returns
to a defined baseline after timers expire, with no panic, deadlock or stale
reference. Passing this milestone is bounded evidence, not a claim of a
security audit or unrestricted protocol conformance.

### N10. Configuration and DHCP

Scope:

- Add a configuration utility for interface state, addresses, routes and
  counters, and a documented persistent configuration format.
- Preserve explicit static configuration and support booting without a NIC or
  without a reachable DHCP server. Network configuration must not block the
  desktop or shell indefinitely.
- Implement DHCP in user space over UDP: discovery, offer/request/acknowledgement,
  option validation, bounded retries, lease renewal/rebinding and expiry.
- Add the required IPv4 broadcast and pre-address UDP behavior deliberately;
  ordinary bound-address checks must not accidentally prevent bootstrap traffic.
- Apply configuration changes coherently, including gateway and DNS information.
  Define address-conflict checks and interface-down/lease-loss behavior.
- Bound mutation authority according to MiniOS's actual single-user permission
  model; do not claim privilege separation the OS does not yet enforce.

Proposed paths: `user/coreutils/net.c`, `user/net/` for the client,
`user/etc/` configuration, startup integration and tests `net_config`, `net_dhcp`.

Exit criteria: static boot, successful lease acquisition, malformed replies,
server absence, renewal, expiry and link recovery are reproducible against a
controlled server. Removing a lease updates routes and resolver configuration
without retaining invalid addresses indefinitely.

### N11. DNS resolution and user-facing tools

Scope:

- Add address conversion and resolver headers/interfaces, including an IPv4
  `getaddrinfo()`/`freeaddrinfo()` subset with explicit unsupported cases.
- Resolve numeric addresses without DNS; support local hosts entries and a
  documented resolver configuration shared with N10.
- Implement bounded DNS queries, transaction/source/question matching,
  defensive name-compression parsing, CNAME limits, retries and TCP fallback
  for truncated responses. Use the N09 random provider where required.
- Deliver useful diagnostics: interface/route/neighbor counters, a controlled
  ICMP echo interface, a small TCP/UDP client/server utility, and a limited HTTP
  client with clearly documented HTTP-only scope.
- Expose ICMP diagnostics through a narrow interface unless a separately scoped
  raw-socket design is accepted; general raw sockets remain deferred.
- Document message flags, options, resolver limitations and errors in manual
  pages. No TLS or certificate-validation claim accompanies the HTTP client.

Proposed paths: `libc/include/{netdb.h,arpa/inet.h,netinet/in.h}`,
`libc/src/net/`, `user/coreutils/`, `user/share/man/` and resolver/tool tests.

Exit criteria: numeric/local-name lookups work without a server; controlled DNS
tests cover positive/negative responses, truncation, compression loops,
unrelated replies, timeout and unavailable servers. Tools exchange data with
independent local peers and display actionable failures.

### N12. Interoperability and release evidence

Scope:

- Run the complete affected networking test set and focused socket, libc,
  thread, descriptor, compositor and modified-driver regressions.
- Exercise one-CPU and four-CPU boots. Use four-vCPU TCG as a reproducible SMP
  configuration and record the QEMU version/backend for every result.
- Measure sustained RX/TX throughput, latency, CPU cost, allocation high-water
  marks, queue drops, retransmissions and recovery after failures. Establish
  baselines before choosing performance thresholds; do not invent targets.
- Test multiple independent connections, bidirectional large transfers, idle
  expiry, repeated server restart, link interruption, and mixed networking with
  desktop/audio/storage activity where shared paths changed.
- Finalize subsystem design, ABI, locking, configuration and troubleshooting
  documentation. Update the main development plan only when implementation
  status is supported by actual results.

Exit criteria: a checked-in result record identifies the tested commit and
configuration, commands, pass/fail counts, captures/logs, performance numbers
and known limitations. Every milestone's exit criterion has evidence or an
explicit unresolved entry; unresolved requirements prevent marking the overall
plan complete.

### N13. TCP window scaling and timestamps

N13 closes the first TCP limitation that the N12 record carried into the
supported feature set. The window scale and timestamp options of RFC 7323
are offered in every SYN and used only when the other SYN of the handshake
carried them too. The receive store grows beyond 65535 bytes so that
scaling has an effect, and the stores stay bounded and are released when
no endpoint can use them. PAWS runs before the sequence check, TS.Recent
follows RFC 7323 section 4.3, and round-trip samples taken from echoed
timestamps feed the existing RFC 6298 estimator. Negotiations, PAWS drops
and samples are counted in the TCP statistics.

The milestone is complete when injected segments with the controlled clock
cover negotiation, fallback, scaled windows in both directions, PAWS,
TS.Recent and timestamp samples, and when a peer that does not share the
guest's parsers observes both options on the wire.

### N14. TCP selective acknowledgements and delayed ACKs

SACK-permitted is negotiated in SYN and SYN ACK like the options of N13.
The receiver reports out-of-order data as SACK blocks by the rules of RFC
2018. The sender keeps a bounded scoreboard of SACKed ranges and recovers
from loss with the pipe and NextSeg rules of RFC 6675, retransmitting only
what the scoreboard shows missing. In-order data is acknowledged after at
most 500 ms and at once for every second full segment, as RFC 1122 and RFC
9293 require, while out-of-order data, duplicates, hole fills and FINs are
acknowledged at once. The scoreboard bound, the reporting limit and the
delay are fixed constants with documented behaviour at their limits.

The milestone is complete when injected segments with the controlled clock
cover block generation, the scoreboard bound, recovery entry, the pipe and
the choice of what to send, the timeout path and the delayed-ACK timer, and
when a scripted peer shows a lost segment repaired from SACK information
without a timeout, correct guest blocks and the delayed ACK on the wire.

### N15. Resolver cache, negative caching and search domains

The resolver keeps DNS answers for the TTL of their records with an upper
bound, keeps negative answers for the time RFC 2308 derives from the SOA
record of the answer, and applies the search list and the ndots option of
`/etc/resolv.conf`. The cache is bounded in entries and lifetime, and the
DHCP client and `net apply` provide the search list.

The milestone is complete when a scripted server that counts queries shows
cached, expired, uncached and capped answers, positive and negative, and
the order in which search candidates are tried.

### N16. DHCP address conflict detection and lease persistence

The DHCP client probes an offered address with ARP as RFC 5227 describes
before it uses it, declines it with DHCPDECLINE on a conflict and
announces it after configuration. The kernel provides the probe through a
narrow `/dev/net` operation, since there are no raw sockets. The lease is
kept on the persistent home volume, and a client that restarts with an
unexpired lease asks for its address again in INIT-REBOOT.

The milestone is complete when injected ARP packets cover the conflict
rules of the kernel, a real conflict on the NIC leads to DHCPDECLINE and a
new address, INIT-REBOOT is acknowledged and refused by the scripted
server and acknowledged by QEMU's server, and probes and announcements
appear in the capture.

## 5. Verification and implementation discipline

- Keep host protocol tests, simulated-device tests and live QEMU/peer tests
  distinct. Report exactly which layer each result validates.
- Use production parsers/state transitions in the deterministic harness; avoid
  a second test-only protocol implementation that can pass independently.
- Give concurrent network tests isolated disks, ports, captures and peer
  processes. Begin with serial test execution (`JOBS=1`) and increase
  concurrency only after resource isolation is verified.
- Rerun the complete affected targeted set after fixing an observed failure.
  Broaden tests when shared code changed or evidence reveals another risk;
  avoid repeatedly running unrelated long regressions.
- Existing useful regression cases include `sockets`, `fdflags`, `evfd`,
  `pthreads` and `comp_core`; select additional cases from the actual changed
  paths. All `net_*` names in this plan are proposed tests.
- Use root-level libc build targets such as `make libc` when ABI work requires
  rebuilding libc and applications. Verify both static and dynamic consumers.
- Keep coherent subsystem boundaries and explanatory ownership/locking comments;
  do not impose an arbitrary source-file line limit.
- Recheck local changes before editing shared files. Treat completion as the
  implementation plus its required tests and documentation, not a commit title
  or a successful build alone.

## 6. Decisions to settle during implementation

The staged architecture above can proceed without selecting every numeric
limit now. Record the following choices before their dependent code lands:

| Decision | Deadline |
|---|---|
| Common socket/file capability representation and legacy Unix ABI policy | N01 |
| Packet capacities, pool budgets, request cancellation and lock order | N02 |
| Exact VirtIO features, wire header layout and reset/configuration handling | N03 |
| Interface control ABI, route policy, ICMP error behavior and initial option subset | N04 |
| Port binding/reuse rules, datagram flags and asynchronous UDP error policy | N05 |
| MSS policy, handshake/accept limits and socket-versus-connection lifetimes | N06 |
| Congestion algorithm, ACK/small-write policy and bounded retry settings | N07 |
| Fragment overlap policy, path-MTU update validation and fallback behavior | N08 |
| Entropy source, initialization readiness and failure policy | N09 |
| Persistent configuration format, DHCP bootstrap and lease-loss policy | N10 |
| Resolver subset, configuration ownership and diagnostic interfaces | N11 |

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
timeout while the retry and progress bounds keep running. Route replacement
and interface down flush the tables. Packet buffers grew from 2048 to 8192
bytes (256 buffers, 2 MiB) so one buffer holds `IPV4_MAX_PACKET`.

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
subset (`libc/src/net/resolv.c`: numeric, `/etc/hosts`, DNS over UDP with
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
store holds 65536 bytes. The stores are allocated per connection and
released as soon as no endpoint can use them, and out-of-order bytes stay
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
presence bitmap. The sender keeps a scoreboard of eight ranges and drops the
highest when a ninth is needed, counts duplicates as ACKs that SACK new
data, decides IsLost per hole, enters recovery at the third duplicate or an
earlier loss, halves the window, and runs the SetPipe and NextSeg loop with
rules 1 to 3 and at most 16 segments per ACK. A timeout keeps the
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
