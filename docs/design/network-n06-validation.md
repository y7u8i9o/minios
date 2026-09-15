# N06 validation, 2026-09-11

N06 adds TCP connection lifecycle to the existing N00–N05 implementation in
`bleeding-edge-net`, based on `87a10c3`. Changes remain uncommitted. Unrelated
working-tree changes were preserved. This record covers establishment and
teardown, with bounded small in-order exchanges. It does not claim N07 reliable
bulk transfer or N09 hostile-network readiness.

## Environment and results

The tests use macOS, QEMU 11.0.3, q35, 512 MiB and TCG. Both one-vCPU and
four-vCPU runs execute cases serially with `JOBS=1`. Native interoperability
uses the real VirtIO network device, QEMU user networking, guest address
10.0.2.15 and a native host TCP listener reached through 10.0.2.2. The host peer
binds an ephemeral localhost port. There is no public service dependency.

| Validation | Result |
|---|---|
| Affected network/socket regression, four vCPUs | 21 passed, 0 failed |
| Same regression, one vCPU | 21 passed, 0 failed |
| Final TCP cases after explicit diagnostic-snapshot error reporting | Four cases on each CPU configuration, all passed |
| Native host interoperability | 20 connections per run, payload and EOF checks passed |
| TCP packet capture | All captured TCP packets passed length/checksum validation, with two ARP packets per run |
| Host peer lifecycle | Success and timeout cleanup passed for raw, user and native TCP peers |
| Header compilation | Passed, with the pre-existing nested-comment warning in `libgui/include/gui/mime.h` |
| Formatting and whitespace | New TCP sources pass clang-format verification, git diff whitespace check passes |

The first controlled, user-ABI and native-host TCP runs passed. Review then
added read-shutdown window advertisement, reset handling for data arriving
after final file close, and explicit TIME_WAIT protection from RST. The full
affected regression was run afterward on both CPU configurations. A final
review made snapshot request failures explicit in the diagnostic API, and the
four TCP cases were rerun on both configurations. Formatting changes
to the existing socket-family test did not alter its behavior.

## What each layer establishes

`net_tcp` uses a fake Ethernet capture device and injected wire segments. Input
passes through production IPv4 and TCP, and output uses production packet
construction and checksums. Tests inspect state and captured wire values for
active/passive and simultaneous open, lost and duplicate handshakes, queue
limits, connect refusal and timeout, poll/SO_ERROR, malformed data offsets and
MSS options, invalid checksums, sequence wrap, RST validation, half-close,
FIN retransmission, simultaneous close, orphan expiry and TIME_WAIT port reuse.
Connection, endpoint and packet-pool accounting returns to baseline. Controlled
timer checks invoke the production callback at its deadline.

`net_tcp_timer` separately advances the controlled clock from outside netd. It
verifies that an endpoint-free TIME_WAIT connection survives until the deadline
and is reclaimed by actual worker timer dispatch at expiry.

`net_tcp_api` exercises real application syscalls over loopback. It checks
blocking and nonblocking connect/accept, poll with SO_ERROR, short address
copyout, accepted descriptor flags, getpeername, read/write, peek, EOF ordering,
write-after-shutdown EPIPE with MSG_NOSIGNAL, signal interruption of accept,
duplicate descriptors, concurrent client/server threads, and process exit with
outstanding data. It does not independently inject a signal during connect.
That path uses the shared interruptible request and endpoint-wait machinery.

`net_tcp_peer` tests guest active open against a native host TCP socket through
VirtIO and QEMU user networking. Each of 20 connections sends 37 or 257 bytes,
half-closes, receives the echoed payload, then receives EOF. The native host
waits for incoming EOF before sending its reply. The post-check verifies the
host's 20 completed exchanges and independently checks all captured IPv4/TCP
lengths and checksums. Guest passive open is covered by the controlled and
loopback cases, not by a host-forwarded native connection.

The peer harness also verifies that terminating a peer blocked in native TCP
accept releases its listener port and PID file after success and timeout.
Its synthetic-QEMU lifecycle tests are separate from live guest evidence.

## Commands and artifacts

The full affected set was run with each of `CPUS=4` and `CPUS=1`:

```sh
JOBS=1 ACCEL=tcg CPUS=4 make test CASES='net_tcp net_tcp_timer net_tcp_api net_tcp_peer net_virtqueue net_ipv4 net_arp net_udp net_nic net_icmp net_udp_peer net_udp_api net_core net_socket net_harness boot sockets sockets_api fdflags evfd pthreads'
JOBS=1 ACCEL=tcg CPUS=1 make test CASES='net_tcp net_tcp_timer net_tcp_api net_tcp_peer net_virtqueue net_ipv4 net_arp net_udp net_nic net_icmp net_udp_peer net_udp_api net_core net_socket net_harness boot sockets sockets_api fdflags evfd pthreads'
```

Final focused checks:

```sh
JOBS=1 ACCEL=tcg CPUS=4 make test CASES='net_tcp net_tcp_timer net_tcp_api net_tcp_peer'
JOBS=1 ACCEL=tcg CPUS=1 make test CASES='net_tcp net_tcp_timer net_tcp_api net_tcp_peer'
make check-net
make check-headers
git diff --check
```

Artifacts are retained under `build/network-n06/`:

- `4cpu/` and `1cpu/` contain full-regression logs and per-case serial logs,
  peer logs, QEMU logs and captures where applicable.
- `final4/` and `final1/` contain the final four TCP cases and their captures.
- `harness/` and `harness.log` contain host peer success/timeout evidence.
- `headers.log` records header compilation output.
- `format.log` records formatting and whitespace verification.
- `source-sha256.txt` identifies the implemented source and test files.

Shared packet, request and device limits still apply. Data recovery, receive
reordering, congestion control, zero-window probes and robust behavior under
sustained loss remain N07. IPv4 fragments/options remain unsupported. Default
sequence and port generation is deterministic until N09.
