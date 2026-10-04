# MiniOS TCP/IP implementation plan

N00–N12 are complete on the branch `bleeding-edge-net` (2026-09-12), and
N13 to N16 are complete on `bleeding-edge-net-options` (2026-09-30).
`network-progress.md` (section 7) contains the evidence and the remaining limitations of each
milestone, and `docs/design/network-n13-n16-validation.md` contains the
release evidence of N13 to N16.

Prepared on 2026-09-10 from source inspection of the working tree on
`bleeding-edge-pkg`, whose HEAD was `1a95395`. The working tree contained
uncommitted work. These findings describe that inspected source, not a tested
networking runtime. Recheck integration points before implementation.

This is a separate plan from the development milestones in `README.md`. Milestone identifiers use the `N`
prefix so they do not renumber the existing development milestones. Current milestone status and validation evidence are recorded in section 7, `network-progress.md`. Proposed paths, commands, and test names are
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
| `kernel/ipc/socket.c` | Unix-domain streams, shared connection rings, descriptor passing, and a file-operations-pointer socket type check | Introduce common socket dispatch and store Unix connection state in its backend. |
| `kernel/syscall/sys_ipc.c` | Socket creation accepts only `AF_UNIX`; the third argument is treated as flags; bind/connect assume Unix addresses; accept ignores address outputs | Add family-aware address handling and real Internet protocol selection without breaking existing Unix callers. |
| `lib/libwire/src/client.c`, `lib/libwire/src/server.c` | MiniOS callers pass socket flags in the third argument | Preserve this legacy Unix convention initially; use standard type flags for new Internet calls. |
| `lib/libc/src/ipc.c` | `send()` and `recv()` call write/read and ignore message flags | Route through socket operations and explicitly validate flags. |
| `kernel/fs/file.c` | Read and write acquire the same position mutex while the backend may block | Add an explicit position-independent I/O path for sockets so a blocked read does not exclude a concurrent write. |
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
operations, error state, references, and a stable poll source. Share descriptor
installation, close-on-exec, and file reference handling. Restrict
`SCM_RIGHTS` processing to Unix sockets.

Internet endpoints use separate UDP and TCP state. A TCP connection may remain
alive after its application file has closed because retransmissions, FIN
processing, or TIME_WAIT still require state. File lifetime, connection
lifetime, and packet lifetime must be accounted for separately.

### Worker and locking model

Use one kernel networking worker initially. It owns protocol state transitions,
route/neighbor updates, and protocol deadlines. System calls exchange copied
data and bounded requests with the worker; short endpoint locks protect the
application-visible queues and readiness state. The worker never waits for a
peer response, application buffer space, or a device descriptor while delaying
unrelated connections.

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
fresh transmit buffers to maintain simple ownership rules.

### Resource policy

Set documented limits for packet storage, sockets, per-socket queued bytes,
pending requests, ARP entries and queued packets, fragments, half-open TCP
connections, accepted connections, and TIME_WAIT state. Define failure behavior
for every limit. Reserve bounded capacity for control traffic under
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
  clock. Maintain protocol logic independent of PCI and user pointers.
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
  reject unsupported combinations. Maintain existing syscall numbers and
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
- Maintain a stable poll-source identity; specify readiness for disconnected,
  connecting, established, failed, and closed endpoints.

Likely paths: `kernel/ipc/socket.c`, `kernel/include/ipc/socket.h`,
`kernel/syscall/sys_ipc.c`, syscall declarations/table, `kernel/fs/file.c`,
`kernel/include/fs/vfs.h`, `kernel/include/minios/abi.h`, kernel/libc errno
headers, `lib/libc/include/sys/socket.h`, `lib/libc/src/ipc.c`, proposed Internet headers.

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
- Maintain transmit memory through completion and implement safe cleanup for
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
  user tools. Separate ordinary socket creation from interface mutation.
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
  start, congestion avoidance and a defined loss-recovery policy. Maintain the
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
- Separate deterministic test injection from normal operation and expose
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

Proposed paths: `lib/libc/include/{netdb.h,arpa/inet.h,netinet/in.h}`,
`lib/libc/src/net/`, `user/coreutils/`, `user/share/man/` and resolver/tool tests.

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
scaling has an effect, and the stores remain bounded and are released when
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
2018. The sender maintains a bounded scoreboard of SACKed ranges and recovers
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

The resolver caches DNS answers for the TTL of their records with an upper
bound, caches negative answers for the time RFC 2308 derives from the SOA
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
stored on the persistent home volume, and a client that restarts with an
unexpired lease asks for its address again in INIT-REBOOT.

The milestone is complete when injected ARP packets cover the conflict
rules of the kernel, a real conflict on the NIC leads to DHCPDECLINE and a
new address, INIT-REBOOT is acknowledged and refused by the scripted
server and acknowledged by QEMU's server, and probes and announcements
appear in the capture.

## 5. Verification and implementation discipline

- Separate host protocol tests, simulated-device tests and live QEMU/peer tests.
  Report exactly which layer each result validates.
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
- Maintain coherent subsystem boundaries and explanatory ownership/locking comments;
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
