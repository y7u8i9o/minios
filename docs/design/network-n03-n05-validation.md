# N03–N05 validation, 2026-09-11

The networking implementation was developed in the `bleeding-edge-net`
working tree based on `87a10c3`. It is not committed by this task. Unrelated
changes already present in the working tree remain separate. This record
covers the restricted N03–N05 feature set in `NETWORK_PLAN.md`; it does not
claim completion of TCP, fragmentation, DHCP, DNS or N09 hardening.

## Environment and execution

- Host: macOS, QEMU 11.0.3.
- Machine: q35, 512 MiB, TCG; four-vCPU and one-vCPU runs.
- NIC: one modern `virtio-net-pci`, MAC `52:54:00:4d:49:4f`.
- Ethernet peer: QEMU dgram backend and the repository's isolated ARP/ICMP/raw
  peer on localhost. UDP interoperability: QEMU user backend and a native
  host UDP echo socket on an ephemeral localhost port.
- All boot cases run serially (`JOBS=1`), with per-case disk copies, ports,
  captures and peer processes. No public service is a test dependency.

The first attempt to bind a peer inside the execution sandbox failed with
`Operation not permitted`; live-peer runs were subsequently executed with
localhost socket access. Initial live RX rejected QEMU's num_buffers=0; the
explicit single-buffer compatibility path corrected this. A later added
asynchronous-error test caught positive errno storage in UDP; storage now
follows the common layer's negative-errno contract and errors notify poll.
The affected test sets were rerun after these fixes.

## Results

| Layer | Configuration | Result |
|---|---|---|
| Full affected kernel/user/driver regression | Four-vCPU TCG, 27 cases below | 27 passed, 0 failed |
| Final networking validation after drop-counter correction | Four-vCPU TCG, 12 cases below | 12 passed, 0 failed |
| Final networking validation | One-vCPU TCG, the same 12 cases | 12 passed, 0 failed |
| Peer harness lifecycle | Fake QEMU, real raw/native peer sockets | Pass: both backend success/timeout paths; unknown backend rejected |
| Header compilation | `make check-headers` | Pass; existing `libgui/include/gui/mime.h` nested-comment warning |
| Kernel build and patch whitespace | Normal warning-as-error kernel build; `git diff --check` | Pass |

The 27-case run:

```sh
JOBS=1 ACCEL=tcg CPUS=4 make test CASES='net_virtqueue net_nic_fail_rx_queue net_nic_fail_tx_queue net_nic_fail_buffers net_nic_fail_started net_ipv4 net_arp net_udp net_nic net_icmp net_udp_peer net_udp_api net_core net_socket net_harness boot sockets sockets_api fdflags evfd pthreads comp_core blk gpu_mode input_tablet input_keyboard audio_pcm'
```

Final network runs used each of `CPUS=1` and `CPUS=4` with:

```sh
JOBS=1 ACCEL=tcg CPUS=1 make test CASES='net_virtqueue net_ipv4 net_arp net_udp net_nic net_icmp net_udp_peer net_udp_api net_core net_socket net_harness boot'
make check-net
make check-headers
```

## What the evidence establishes

`net_virtqueue` runs the production split-ring completion path with simulated
rings: 66000 completions cross 16-bit index wrap, all descriptors can be
exhausted and reclaimed, and invalid/duplicate IDs or an impossible used-index
advance cannot index unchecked memory. Network-header tests cover short and
oversized completions, offload flags and merged-buffer rejection. These are
simulated-device results, not injected malformed completions from real QEMU.

`net_nic_fail_rx_queue`, `net_nic_fail_tx_queue`, `net_nic_fail_buffers` and
`net_nic_fail_started` inject failure in actual initialization and check that
queue and DMA allocations are released without publishing an interface.
`net_nic` sends and receives 640 raw frames through the real device, then
resets it and checks packet-pool accounting. The peer records exactly 640
frames and 640 echoes.

The deterministic IPv4 tests exercise the production route/ARP/ICMP paths
under netd: off-subnet traffic resolves the gateway MAC, malformed headers,
fragments and options are rejected, MTU/missing-route errors are explicit,
neighbor queues fill predictably, retries expire, and queued buffers return
to the pool. A connected UDP endpoint receives `EHOSTUNREACH` after three
unanswered ARP attempts and `ENETDOWN` when the interface goes down.

The controlled ICMP run captures two ARP packets and four ICMP packets,
covering an echo request and reply in each direction. Independent capture
validation checks the IPv4 and ICMP lengths/checksums.

The UDP kernel and user tests establish datagram boundaries, wildcard versus
specific bindings, distinct-address port sharing, connected filtering, empty
datagrams, receive-ring limits, invalid and zero checksums, source addresses,
peek/truncation, oversized-send errors, connected ICMP error consumption,
unread-queue cleanup, and repeated port reuse. User tests guard bytes beyond
a short iovec while `MSG_TRUNC` reports the full datagram length, exercise a
blocked reader and concurrent writer, and cover descriptor duplication and
Internet read/write copy buffers.

The independent native-host UDP run exchanges 300 datagrams: 100 empty,
100 of 37 bytes, and 100 of 1472 bytes. The capture contains 600 UDP packets;
all lengths and nonzero pseudo-header checksums pass the independent capture
checker. This is interoperability with the host socket stack via QEMU user
networking, distinct from simulated parser tests and raw-frame echo.

## Artifacts and limits

Preserved artifacts are under `build/network-n03-n05/`:

- `4cpu-regression/`: run log and per-case serial logs; live cases also retain
  peer logs, QEMU diagnostics and captures.
- `1cpu/`: final one-CPU run and corresponding per-case evidence.
- `4cpu-network/`: final four-CPU network rerun after the IPv4 drop counter
  was corrected to count rejected packets instead of all default-handler input.
- `harness.log`, `headers.log`, and `source-sha256.txt`: host checks and source
  fingerprints for the final implementation.

Successful real reset and partial initialization cleanup were exercised.
Hardware refusal to reset was not injected; the error path intentionally
pins DMA-visible memory. Link changes are not reported because STATUS is not
negotiated. Automatic restart/hotplug are absent. Broadcast/multicast,
fragments/options, TCP, DHCP and DNS are not supported by these milestones.
Ports are deterministic and ICMP errors use tuple matching; hostile-network
hardening and unpredictability remain N09 requirements. No throughput,
latency or release-readiness claim is inferred from these correctness tests.
