# Memory balloon (V4 of the 0.6.0 release)

The host gives memory to the guest and takes memory back through
virtio-balloon (device 5). The driver is
`kernel/drivers/virtio/virtio_balloon.c`. It accepts the modern PCI device
0x1045 and the transitional device 0x1002 with modern capabilities. It
negotiates `VIRTIO_BALLOON_F_STATS_VQ` and
`VIRTIO_BALLOON_F_DEFLATE_ON_OOM`. It does not implement free page
reporting or free page hinting. QEMU offers neither feature by default.

## Target and size

The host writes the target size of the balloon in pages of 4 KiB to
`num_pages` of the configuration space. The QMP command `balloon` sets the
memory that the guest is to retain, and QEMU computes the target from it.
The new value raises the configuration interrupt. The callback
`config_changed` of the virtio core (`display.md`) records the target and
wakes the thread `balloon`.

The thread moves the size towards the target in requests of up to 256
pages. A request is a buffer of 32-bit frame numbers.

- Inflation allocates free pages with `pmm_alloc`, sends their frame
  numbers on the inflate queue and waits for the host. The pages then go
  to the list of the balloon, and `pmm_adjust_total` removes them from the
  total of the allocator. Inflation leaves 1024 pages (4 MiB) free. A
  target beyond that limit stops the inflation with the warning
  "inflation stopped at N of M pages, free memory is low". The thread then
  tries again every second.
- Deflation takes pages from the list, sends their frame numbers on the
  deflate queue, frees them and adds them to the total again.

After every request the driver writes the size to `actual`. QEMU reports
the guest memory as the machine memory minus `actual` in `query-balloon`.
When the size reaches the target the driver logs `N pages, the target`.

## Statistics

The statistics queue contains one buffer. The driver adds the buffer once
at start. The host returns the buffer when it wants new values, at the
interval of the QOM property `guest-stats-polling-interval`. The thread
then fills the buffer again and adds it to the queue. QEMU reads the
values when the buffer arrives and shows them in the QOM property
`guest-stats`.

| Tag | Value |
|---|---|
| `SWAP_IN`, `SWAP_OUT` | bytes swapped in and out since boot (`swap_get_stats`) |
| `MAJFLT`, `MINFLT` | faults resolved from swap or a file and from memory (`vma_get_fault_counts`) |
| `MEMFREE` | free memory in bytes |
| `MEMTOT` | the total of the allocator in bytes, without the balloon |
| `AVAIL` | free memory and the cached file pages in bytes |
| `CACHES` | the cached file pages in bytes (`filemap_get_stats`) |

The fault counters are systemwide atomic counters of `mm/vma.c` beside
the counters of each process.

## Deflation under memory pressure

With `VIRTIO_BALLOON_F_DEFLATE_ON_OOM` the driver registers the pressure
source of the allocator (`pmm.md`). The allocator asks the source for
pages when `pmm_alloc` finds no free block. kswapd asks it before an
eviction, and `swap_alloc_user_frame` asks it before it waits for kswapd
(`swap.md`). The source frees at least 256 pages of the balloon at once
and adds them to the total. The thread of the balloon itself never
receives pages from the source. Its own allocations would otherwise empty
the balloon.

QEMU does not offer `VIRTIO_BALLOON_F_MUST_TELL_HOST`. The guest therefore
uses the pages before the host learns of the deflation. The source records
the frame numbers of up to 1024 released pages. The thread sends them on
the deflate queue afterwards and writes the new size to `actual`. Pages
beyond the 1024 entries are released without a report, and the thread
logs their number. The host learns the size from `actual` in both cases.

A release under pressure stops the inflation until the host sets a new
target. Otherwise the balloon would take again the pages that the system
needs.

## Interface

`/dev/balloon` (mode 0444) reports the state as text:

    BalloonSize: 131072 kB
    BalloonTarget: 131072 kB
    DeflateOnOOM: no
    ReleasedOnPressure: 0 kB
    StatsUpdates: 2

`virtio_balloon_get_info` returns the same values to the kernel. The device
appears as `virtio-balloon` in the PCI list of `/dev/devices`.

## Locks

`balloon.lock` protects the list of pages, the size, the target, the
frame numbers of the report, the counters and the flags of the thread. The
pressure source takes it in any context. The thread never allocates memory
and never takes the lock of a virtqueue while it has acquired the lock.
`docs/design/locking.md` lists the orders.

## Tests

The runner attaches `-device virtio-balloon-pci,id=balloon0` for a case
with a `balloon` file. The file contains further device options.

- `balloon` (512 MiB): the QMP script sets the polling interval of the
  statistics to 1 s and the guest memory to 384 MiB. The kernel test
  (`kernel/tests/test_balloon.c`) waits for a balloon of 32768 pages and
  requires `MemTotal` of `/dev/meminfo` to shrink by 128 MiB and
  `/dev/balloon` to report the size and the target. It waits for two
  statistics updates. The script then restores 512 MiB, and the test
  requires the deflation and the original `MemTotal`. The `post` script
  requires `query-balloon` to report 402653184 and then 536870912 bytes,
  `stat-total-memory` of `guest-stats` to equal the `MemTotal` that the
  guest printed, and a positive `stat-free-memory` and `last-update`.
- `balloon_oom` (512 MiB, `deflate-on-oom=on`, a swap device of 64 MiB):
  the script leaves 192 MiB to the guest, a balloon of 81920 pages. The
  test allocates 64 MiB more than is free with `pmm_alloc_page`. Every
  allocation must succeed, and the balloon must release at least 16384
  pages. The test frees the pages and requires the balloon to remain
  smaller. The program `memtouch` (`user/tests/memtouch.c`) then writes
  and reads 32 MiB more than is free. The test requires its success
  without a page swapped out. The `post` script requires `query-balloon`
  to report 512 MiB minus the last size that the guest printed.

Both cases pass on x86_64 and aarch64.
