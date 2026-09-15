# N10–N12 validation, 2026-09-12

N10 (configuration and DHCP), N11 (name resolution and tools) and N12
(release evidence) complete the network plan on `bleeding-edge-net`, on the
uncommitted tree based on `87a10c3`. Unrelated working-tree changes were
preserved. This record is the release evidence the plan asks for: the tested
configuration, the commands, the pass counts, the throughput baseline and the
limitations that remain documented rather than resolved.

## Configuration

macOS 27.0 beta on Apple silicon, QEMU 11.0.3, q35, 512 MiB, TCG, cases run
serially (`JOBS=1`), once with four vCPUs and once with one. Every case
attaches `virtio-rng-pci` backed by `/dev/urandom`. Native interoperability
uses the VirtIO NIC with QEMU user networking: guest 10.0.2.15, gateway and
DHCP server 10.0.2.2, name server 10.0.2.3, host TCP peers on ephemeral
localhost ports. No public service is a dependency.

## Results

| Run | Result |
|---|---|
| 39 cases, four vCPUs | 39 passed, 0 failed |
| 39 cases, one vCPU | 39 passed, 0 failed |
| `make check-net`, `make check-headers`, `git diff --check` | pass (the header check keeps the pre-existing nested-comment warning in `libgui/include/gui/mime.h`) |
| `make check-net-fuzz` (N09 record) | three seeds of 30 s, no finding |

The 39 cases are the 26 network cases (`net_dns net_tools net_dhcp
net_tcp_transfer net_tcp_bulk net_fragment net_path net_pressure net_random
net_random_unavailable net_random_zero net_tcp net_tcp_timer net_tcp_api
net_tcp_peer net_virtqueue net_ipv4 net_arp net_udp net_nic net_icmp
net_udp_peer net_udp_api net_core net_socket net_harness`) and the shared-path
regressions `boot sockets sockets_api fdflags evfd pthreads pipes comp_core
comp_data comp_seat comp_panel gui_app blk`. The compositor and GUI cases boot
init, which now starts `net apply` in the background; they pass without a
NIC, which covers the no-network boot path.

## Baseline measurements

`net_tcp_bulk` (256 KiB each direction with a native host peer, 4 KiB reads,
16 KiB writes, 4096-byte receive window, no window scaling):

| vCPUs | Elapsed | Throughput per direction | Retransmissions |
|---|---|---|---|
| 4 | 178 ms | 1437 KiB/s | 0 |
| 1 | 128 ms | 2000 KiB/s | 0 |

These are TCG numbers on one host and are a baseline, not a target. The
receive window bounds throughput on longer paths; window scaling is a
documented omission. `net_pressure` records the packet low-water mark at the
32-buffer control reserve after three exhaustion cycles, and `net_fragment`
records the reassembly high-water mark at all eight contexts, both returning
to baseline.

## What each new layer establishes

`net_dhcp` drives the production client binary against a scripted loopback
server (absence, malformed and unrelated replies, application through
`/dev/net` and `/etc/resolv.conf`, renewal at T1 with `ciaddr`, NAK,
rediscovery, expiry) and then obtains a live lease from QEMU's DHCP server
over the NIC. `net_dns` drives the libc resolver against a scripted loopback
server for positive, negative, failure, compression-loop, truncated (TCP
fallback), CNAME, chain-limit, unrelated-reply and silent-server cases, plus
numeric and hosts-file lookups without a server. `net_tools` runs `net`,
`ping`, `nc` and `http` as separate processes against the gateway and
loopback servers and checks their output and exit statuses.

## Commands

```sh
C='net_dns net_tools net_dhcp net_tcp_transfer net_tcp_bulk net_fragment net_path net_pressure net_random net_random_unavailable net_random_zero net_tcp net_tcp_timer net_tcp_api net_tcp_peer net_virtqueue net_ipv4 net_arp net_udp net_nic net_icmp net_udp_peer net_udp_api net_core net_socket net_harness boot sockets sockets_api fdflags evfd pthreads pipes comp_core comp_data comp_seat comp_panel gui_app blk'
JOBS=1 ACCEL=tcg CPUS=4 make test CASES="$C"
JOBS=1 ACCEL=tcg CPUS=1 make test CASES="$C"
make check-net
make check-headers
git diff --check
```

Artifacts are under `build/network-n10-n12/`: `all-4cpu.log`, `all-1cpu.log`,
`headers.log`, `harness.log`; per-case serial logs, QEMU logs, peer logs and
captures of the last run are under `build/tests/<case>/`.

## Limitations carried into the supported feature set

- TCP: no window scaling, SACK, timestamps or delayed ACKs; scripted loss
  with a native peer is not in the suite (recovery is verified with injected
  segments and the controlled clock).
- IPv4: no fragmented exchange with a native peer, DF never set on UDP, no
  IP options, no forwarding, no multicast or directed broadcast.
- DHCP: no address-conflict detection, one interface, no persistent lease.
- Resolver: IPv4 only, numeric services, no search domains, caching or
  reverse lookups. `http` is HTTP/1.0 over plain `http://` with no TLS.
- Randomness: one boot seed, no reseeding, global rate limits.
- Formatting: no clang-format configuration exists in the repository, so no
  formatting claim is made; whitespace is checked with `git diff --check`.
