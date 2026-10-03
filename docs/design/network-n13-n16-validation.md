# N13–N16 validation, 2026-09-30

N13 (TCP window scaling and timestamps), N14 (selective acknowledgements
and delayed ACKs), N15 (resolver cache, negative caching and search
domains) and N16 (DHCP address conflict detection and lease persistence)
close the limitations that the N10–N12 record carried into the supported
feature set, except those listed at the end. They were implemented on
`bleeding-edge-net-options`, based on `d1e7689`, one commit per milestone.
Two further commits fix defects that the validation runs found. The first
is a defect of the base, in which a single-CPU boot read the local APIC
before it was mapped and every one-vCPU boot failed; the second is a FIN
that was refused in a closed receive window. A third commit rewrites the
comments of N13 to N15 as full sentences without changing code. This
record lists what was run, what each layer establishes and what remains.
It is bounded evidence, not a conformance claim.

## Configuration

macOS 27.0 beta on Apple silicon, QEMU 11.0.3, q35, 512 MiB, TCG, cases run
serially (`JOBS=1`), once with four vCPUs and once with one. Every case
attaches `virtio-rng-pci` backed by `/dev/urandom`. Native interoperability
uses the VirtIO NIC with QEMU user networking (guest 10.0.2.15, gateway and
DHCP server 10.0.2.2, name server 10.0.2.3). The scripted TCP peer uses the
dgram backend on an isolated link (guest 10.0.0.2, peer 10.0.0.1). No
public service is a dependency.

## Results

| Run | Result |
|---|---|
| 23 cases, four vCPUs | 23 passed, 0 failed |
| 23 cases, one vCPU | 23 passed, 0 failed |
| `make check-net` | pass |
| `make check-net-fuzz` | three seeds of 30 s, ASan and UBSan, no finding |
| `make check-headers` | pass, with the pre-existing nested-comment warning in `libgui/include/gui/mime.h` |
| `git diff --check` | pass |

The 23 cases are the TCP cases (`net_tcp net_tcp_api net_tcp_bulk
net_tcp_options net_tcp_options_peer net_tcp_sack net_tcp_peer
net_tcp_timer net_tcp_transfer net_xfer net_xfer_host net_pressure
net_path`), the resolver cases (`net_dns net_dns_cache net_tools`), the
DHCP and ARP cases (`net_dhcp net_arp net_arp_probe net_ipv4 net_icmp`),
`net_udp_peer`, whose capture passes through the extended capture
checker, and `boot`. `net_tcp_options`, `net_tcp_options_peer`,
`net_tcp_sack`, `net_dns_cache` and `net_arp_probe` are new.

Fuzz iterations per 30-second seed; `tcp_options` counts accepted segments
that carried a window scale, timestamp, SACK-permitted or SACK option:

| Seed | Iterations | TCP segments accepted | With options | IPv4 headers accepted |
|---|---|---|---|---|
| 0x7090809 | 36605280 | 3120431 | 84007 | 16870679 |
| 0xffffffff | 36746226 | 3133342 | 84903 | 16937813 |
| 0x123456789abcdef | 36627132 | 3125054 | 84409 | 16883158 |

## Measurements

`net_tcp_bulk` (256 KiB each direction with QEMU's user-mode stack, which
offers no option, so the connection falls back to an unscaled window of
65535 bytes instead of the 4096 bytes of N07):

| vCPUs | Elapsed | Throughput per direction | Retransmissions | Delayed ACKs, sent by the timer |
|---|---|---|---|---|
| 4 | 34 ms | 7529 KiB/s | 0 | 61, 0 |
| 1 | 31 ms | 8257 KiB/s | 0 | 76, 0 |

The N12 baseline was 178 ms and 128 ms. Before the closed-window fix the
one-vCPU run took 1116 ms, 1082 ms of which passed between the last data
and the retransmission of QEMU's FIN. `net_tcp_options_peer` recorded, in
both runs, a largest flight of 16656 bytes, 75 guest segments that ended
beyond the edge an unscaled window would give, a guest window of 131072
bytes, the dropped segment retransmitted 1 ms of guest time after the
original with no SACKed segment sent again, one retransmission in total
and no timeout, and a delayed ACK after 101 ms. These are TCG numbers on
one host and are a baseline, not a target; the recorded values are those
of the final runs.

## What each layer establishes

`net_tcp_options` and `net_tcp_sack` (kernel, controlled clock, fake
capture interface) drive production parsing, output and timers with
injected segments and decode the captured output with a decoder of their
own. They establish negotiation and fallback, scaled windows in both
directions, the payload reduction by the timestamp option, PAWS with the
24-day rule, TS.Recent, timestamp RTT samples including after a
retransmission, SACK block generation and ordering within the option
budget, the scoreboard bound, recovery entry at the third duplicate, the
pipe and the choice between retransmission and new data, the timeout path
over the scoreboard, the delayed-ACK timer, the ACK for every second full
segment, receiver silly window avoidance and the FIN in a closed window.
The existing `net_tcp`, `net_tcp_transfer`, `net_path` and `net_pressure`
cases establish that the N06–N09 behaviour remains correct with the larger stores.

`net_tcp_options_peer` (kernel over the real NIC, dgram backend, scripted
peer in `tools/netpeer/scripted.c`) establishes the same options on the
wire against code that shares nothing with the guest: the peer builds and
parses its own headers, drops one guest segment, reorders its own data,
sends a segment with an old timestamp and times the delayed ACK. It is a
scripted peer, not an independent TCP implementation.

`net_tcp_bulk`, `net_tcp_peer`, `net_xfer_host` and `net_tools` establish
interoperability with QEMU's user-mode stack and, through it, the host's
sockets. Since that stack offers only MSS, they show the fallback, which
the capture checker requires for `net_tcp_bulk` and `net_tcp_peer`.
`check_capture.py` validates every TCP capture on its own: timestamps
exactly on the connections that negotiated them, every echo a value the
other side sent, no data beyond the scaled window, and SACK blocks only on
SACK connections and within the data the other side sent.

`net_dns_cache` and `net_dns` drive the libc resolver against a scripted
loopback server that counts queries. `net_arp_probe` drives the kernel's
probe slots with injected ARP. `net_dhcp` drives the production client
against a scripted loopback server while its probes go out on the NIC,
where QEMU's answer for 10.0.2.2 provides a real conflict, and then
against QEMU's DHCP server, including INIT-REBOOT; its capture shows 25
probes and 16 announcements.

## Commands

```sh
C='net_tcp net_tcp_api net_tcp_bulk net_tcp_options net_tcp_options_peer net_tcp_sack net_tcp_peer net_tcp_timer net_tcp_transfer net_xfer net_xfer_host net_pressure net_path net_dns net_dns_cache net_tools net_dhcp net_arp net_arp_probe net_ipv4 net_icmp net_udp_peer boot'
JOBS=1 ACCEL=tcg CPUS=4 make test CASES="$C"
JOBS=1 ACCEL=tcg CPUS=1 make test CASES="$C"
make check-net
make check-net-fuzz
make check-headers
git diff --check
```

Artifacts are under `build/network-n13-n16/`: `all-4cpu.log`,
`all-1cpu.log`, `harness.log`, `headers.log` and `fuzz.log`, and per case
the serial log, the peer log and the capture of each run in `cases-4cpu/`
and `cases-1cpu/`.

## Limitations carried into the supported feature set

This list replaces the one of the N10–N12 record.

- TCP has no D-SACK, no rescue retransmission (RFC 6675 rule 4), no ECN,
  no Nagle algorithm and no TCP Fast Open. Loss with a native host stack is
  not scripted, because QEMU's user-mode stack neither drops on request nor
  offers the options; loss recovery is verified with injected segments and
  with the scripted peer on the raw link.
- IPv4 has no fragmented exchange with a native peer, never sets DF on
  UDP, and has no IP options, forwarding, multicast or directed broadcast.
- The DHCP client serves one interface. A configured address is not
  defended against a later conflict (RFC 5227 section 2.4), and a static
  configuration is not probed.
- The resolver is IPv4 only, with numeric services and no reverse lookups,
  and each process has its own cache, which is not shared. `http` is
  HTTP/1.0 over plain `http://` with no TLS.
- The random provider has one boot seed and no reseeding, and the rate
  limits are global.
- No clang-format configuration exists in the repository, so no formatting
  claim is made; whitespace is checked with `git diff --check`.
