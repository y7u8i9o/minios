# PCI, virtio-blk and the block layer

## PCI

`drivers/pci.c` enumerates every bus, slot and function through the
configuration space ports `0xcf8` and `0xcfc`. Each function found is
recorded in a static table of `struct pci_dev` with vendor, device, class
codes, interrupt line and pin and the decoded base address registers
(64 bit memory BARs consume two slots). The table is filled once at boot
and read without a lock afterwards. Helpers provide configuration space
accesses of 8, 16 and 32 bits, capability list walks, bus master enabling
and MSI-X setup: `pci_msix_enable` sets the enable bit of the MSI-X
capability and `pci_msix_set_vector` maps the table BAR and writes the
local APIC address and the vector into one table entry.

QEMU q35 shows seven functions: host bridge, VGA, e1000, the virtio-blk
function, the ISA bridge, AHCI and SMBus. They are logged at boot.

## virtio

`drivers/virtio/virtio.c` implements the modern (non legacy) PCI
transport. The vendor specific capabilities describe the common
configuration, the notification area, the ISR status and the device
configuration structures inside the BARs; each is mapped with
`vmm_map_mmio`. Initialization follows the specification order: reset,
ACKNOWLEDGE, DRIVER, feature negotiation with `VIRTIO_F_VERSION_1`
required, FEATURES_OK, queue setup, then DRIVER_OK.

A `struct virtqueue` is a split ring of up to 128 entries occupying two
physically contiguous pages: descriptors and the available ring in the
first, the used ring in the second. Descriptors are kept on a free list
through their `next` fields. `virtq_alloc_chain` takes a chain of n
descriptors, `virtq_submit` publishes the head in the available ring
with a memory fence and writes the queue index to the notification
address. Every queue keeps a completion cookie per head descriptor.

Interrupts arrive through MSI-X. Each device gets one vector from 40
upwards, registered with `irq_register`; the configuration and every queue
use table entry 0. The handler walks the used ring of every queue under
`virtqueue.lock`, calls the driver completion callback for each finished
chain, frees the chain and wakes the queue wait queue.

## virtio-blk

`drivers/virtio/virtio_blk.c` probes device ids `0x1001` (transitional)
and `0x1042` (modern only), negotiates `VIRTIO_BLK_F_FLUSH` when offered
and registers `vda`, `vdb`, ... with the block layer. A request is a three
descriptor chain: the 16 byte header (type, sector), the data buffer and
the status byte. The caller sleeps on the queue wait queue until the
completion callback marks the request done, holding `virtqueue.lock` as
the condition lock. Transfers are split into 8 KiB pieces and bounced
through a `kmalloc` block so the device always sees physically contiguous
memory regardless of the caller's buffer.

## Block layer

`block/blockdev.c` keeps the list of `struct blockdev`: name, sector size,
sector count and the driver's `rw` and `flush` callbacks.
`blockdev_register` also creates `/dev/<name>` in devfs. The device file
supports byte addressed reads and writes through the block cache and
`lseek` against the device size.

`block/bcache.c` is a write back cache of 256 buffers of 4 KiB, eight
sectors each. `bread` returns a buffer locked by its mutex and filled from
disk if it was not valid; `bwrite` marks it dirty; `brelse` unlocks and
drops the reference. Buffers sit on one LRU list; a miss evicts the least
recently used unreferenced clean buffer, or writes back the oldest dirty
one and retries when no clean buffer exists. `bcache_sync` writes every
dirty buffer of one device or of all devices and then issues the device
flush. The `sync` system call runs the filesystem sync hooks and then
`bcache_sync(NULL)`.

M36 adds pinned buffers for the mfs journal: `bpin` marks a locked buffer
dirty and pinned and takes a reference, so neither eviction nor
`bcache_sync` writes it; the filesystem writes it itself with `bwrite_now`
once the journal holds a copy and releases it with `bunpin`. `bforget`
drops the contents of a buffer and `bcache_discard` forgets every
unreferenced buffer of a device; both exist for the simulated crashes of
the journal test.

Locks: `bcache_lock` protects the LRU list and the identity, reference
count and flags of every buffer; `buf.lock` is a mutex held between
`bread` and `brelse` and during write back. `virtqueue.lock` protects the
ring bookkeeping and is taken from the interrupt handler. See
`locking.md` for the ordering.

## Disk image and tests

`make disk` creates `build/disk.img`, a 512 MiB mfs image (`DISK_MB`
overrides the size). `make run` and `make gdb` attach it as
`virtio-blk-pci`. The test runner copies the image for every case so
cases start from identical contents and do not affect each other; a case
may ship its own `disk.img`. A case file `swap` adds a zero filled swap
device, `mfs2` an empty mfs image of the given size in MiB (`DISK2` in
the post script) and `fat` one FAT image per line (`<size_mb> <12|16|32>
[dir]`, built by `mkfat`; `FATIMG` and `FATIMGS` in the post script), in
that order after the root disk.

The `blk` case checks the geometry reported by the device, raw single
sector, 128 sector and past the end transfers, delayed write back through
the cache, LRU eviction with more blocks than buffers, write back of an
evicted dirty buffer, and `/dev/vda` reads and writes across block
boundaries through the VFS followed by `sync`.
