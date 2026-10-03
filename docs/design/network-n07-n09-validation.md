# N07–N09 validation, 2026-09-12

N07 (reliable TCP transfer), N08 (IPv4 reassembly and path MTU) and N09
(entropy, input robustness and resource recovery) were implemented on
`bleeding-edge-net` on top of the uncommitted N03–N06 tree based on
`87a10c3`. The implementation remains uncommitted; unrelated working-tree
changes were preserved. This record lists what was run, what each layer
establishes and what remains untested. It is bounded evidence, not a
conformance or security claim.

## Environment and results

macOS 27.0 beta on Apple silicon, QEMU 11.0.3, q35, 512 MiB, TCG, cases run
serially (`JOBS=1`). Native interoperability uses the real VirtIO NIC, QEMU
user networking, guest address 10.0.2.15 and a native host TCP listener on an
ephemeral localhost port reached through 10.0.2.2. Every case attaches a
`virtio-rng-pci` device backed by `/dev/urandom` unless its case directory
contains `no-rng` or `rng-zero`. No public service is a dependency.

| Validation | Result |
|---|---|
| New cases, four vCPUs | 8 passed, 0 failed |
| New cases plus the 21-case network/socket regression, one vCPU | 29 passed, 0 failed |
| 21-case network/socket regression, four vCPUs | 21 passed, 0 failed |
| Native bulk transfer | 262144 bytes each direction, byte-exact, EOF observed; capture 766 TCP and 2 ARP packets, all checksums and lengths valid |
| Host fuzzing, `make check-net-fuzz` | Three seeds, 30 s each, ASan and UBSan, no finding; ChaCha20 known answer passes |
| Pressure recovery | Three cycles return to empty TCP tables and the packet baseline; low-water mark 31 of 256 buffers, at the control reserve |
| Reassembly pressure | Context high-water 8 of 8, all released by the worker timer, pool restored |
| Header compilation | Passed, with the pre-existing nested-comment warning in `libgui/include/gui/mime.h` |
| Whitespace | `git diff --check` passes |

Fuzz iterations per 30-second seed:

| Seed | Iterations | TCP segments accepted | IPv4 headers accepted |
|---|---|---|---|
| 0x7090809 | 34546487 | 1564495 | 15923954 |
| 0xffffffff | 35720364 | 1615760 | 16465442 |
| 0x123456789abcdef | 35802354 | 1619934 | 16500357 |

The first `make check-net-fuzz` attempt on this host never reached `main`:
with `CC=/usr/bin/clang` in the environment the runner used Apple clang 17,
whose AddressSanitizer runtime recurses through `malloc` in its own shadow
initialization on this macOS beta. The runner now prefers Homebrew LLVM
(clang 22.1.8 here), ignores `CC`, probes a one-second run with a bounded
wait and falls back to UBSan alone if the address sanitizer does not start.
The recorded results are from the Homebrew toolchain with both sanitizers.

The repository contains no clang-format configuration, so no formatting claim
is made for these sources; the earlier N06 record's formatting row applied
a style that was not written down.

## What each layer establishes

`net_tcp_transfer` (kernel, controlled clock, fake capture interface) drives
production parsing, output and timers with injected segments: partial write
of 8229 bytes into the 8192-byte buffer, the initial burst limited by the
congestion window to 1200 bytes, a partial ACK across 32-bit wraparound
leaving the exact suffix, an RTT sample and slow start, three duplicate ACKs
entering recovery with window one MSS and threshold at least two MSS and no
sample in progress, a timeout doubling the RTO to 2 s and retaining the bytes,
complete acknowledgement, a zero window retaining a 37-byte write and probed
at `snd_una - 1`, resumption on the window update, and out-of-order data
plus FIN before a hole delivered as the exact stream `abcdefgh` followed by
EOF, with the connection released afterwards.

`net_tcp_bulk` (kernel over the real NIC and a native host peer) sends 256
KiB in 16 KiB writes, half-closes, receives the echoed 256 KiB in 4 KiB reads
and EOF, verifying every byte against the generator. The host peer records
`tcp stream 1 bytes 262144`; `check_capture.py` validates every captured
packet independently. This is the interoperability evidence for N07; it
does not include scripted loss, because QEMU user networking cannot drop or
reorder on request.

`net_fragment` (kernel, controlled clock) reassembles a 3000-byte UDP
datagram through production IPv4, UDP and socket delivery from reverse-order
fragments and an exact duplicate, checks that a conflicting overlap rejects
and quarantines the datagram, fills all eight contexts and observes
`fragment_full`, rejects an oversized offset before any copy, advances the
clock past the 30-second deadline and lets the real worker timer release the
contexts, then reassembles again and checks the packet pool.

`net_path` (kernel, controlled clock) sends 1000 bytes, forges the quoted
sequence and observes rejection, replays the exact quote and observes the
path at 600 and the MSS at 560, expires the retransmission at the new size,
expires the cache after ten minutes, then on a second connection lets two
timeouts pass without ICMP and observes the 536-byte fallback, continues to
`ETIMEDOUT`, and finally replaces the route and observes both tables empty.

`net_random`, `net_random_unavailable` and `net_random_zero` boot the same
kernel with a working entropy device, without one, and with `/dev/zero`
entropy. The first draws 64 distinct values; the other two check the
warning, `EAGAIN` from the provider without a substituted value, and
`EAGAIN` from TCP connect, TCP listen and automatic UDP binding while
socket creation remains available.

`net_pressure` (kernel, controlled clock, three cycles) is described in
`network.md`; each cycle ends with the TIME_WAIT table full and no endpoint,
and after 120 controlled seconds the tables and pool are empty again.

The regression set (`net_tcp net_tcp_timer net_tcp_api net_tcp_peer
net_virtqueue net_ipv4 net_arp net_udp net_nic net_icmp net_udp_peer
net_udp_api net_core net_socket net_harness boot sockets sockets_api fdflags
evfd pthreads`) establishes that the N01–N06 behaviour, the Unix socket and
descriptor paths and the peer harness are unchanged.

## Commands and artifacts

```sh
JOBS=1 ACCEL=tcg CPUS=4 make test CASES='net_tcp_transfer net_tcp_bulk net_fragment net_path net_pressure net_random net_random_unavailable net_random_zero'
JOBS=1 ACCEL=tcg CPUS=4 make test CASES='net_tcp net_tcp_timer net_tcp_api net_tcp_peer net_virtqueue net_ipv4 net_arp net_udp net_nic net_icmp net_udp_peer net_udp_api net_core net_socket net_harness boot sockets sockets_api fdflags evfd pthreads'
JOBS=1 ACCEL=tcg CPUS=1 make test CASES='net_tcp_transfer net_tcp_bulk net_fragment net_path net_pressure net_random net_random_unavailable net_random_zero net_tcp net_tcp_timer net_tcp_api net_tcp_peer net_virtqueue net_ipv4 net_arp net_udp net_nic net_icmp net_udp_peer net_udp_api net_core net_socket net_harness boot sockets sockets_api fdflags evfd pthreads'
make check-net-fuzz
make check-net
make check-headers
git diff --check
```

Artifacts are retained under `build/network-n07-n09/`:

- `new-cases-4cpu.log`, `regression-4cpu.log` and `all-1cpu.log` are the
  runner outputs; the per-case serial logs, QEMU logs, peer logs and
  captures of the last run are under `build/tests/<case>/`.
- `fuzz.log` contains the fuzz output; `build/network-fuzz/` the binary.
- `headers.log` and `format.log` record the header check and the
  formatting probe.
- `source-sha256.txt` identifies the implemented source and test files.

## Limits carried forward

No window scaling, SACK, timestamps or delayed ACKs, so throughput is
bounded by the 4096-byte receive window. Loss with a scripted native peer,
fragmented traffic with a native peer and DF on UDP are untested or
unimplemented. The random provider is seeded once per boot. Rate limits are
global. N10 (configuration and DHCP) and N11 (DNS and tools) may proceed;
N12 owns the broader interoperability evidence.
