# PCI, virtio-blk and the block layer

## PCI

`drivers/pci.c` enumerates the slots and functions of bus 0 and of every
bus that a PCI to PCI bridge found earlier names as its secondary bus. The
firmware numbers the buses below a bridge after the bridge's own, so one
pass in bus order reaches every function without reading absent buses.
Configuration space accesses go through `platform_pci_read32` and
`platform_pci_write32`: the ports `0xcf8` and `0xcfc` on the PC, the ECAM
window of the device tree on aarch64 (`arch.md`). Each function found is
recorded in a static table of `struct pci_dev` with vendor, device, class
codes, interrupt line and pin and the decoded base address registers
(64 bit memory BARs consume two slots). The table is filled once at boot
and read without a lock afterwards. Helpers provide configuration space
accesses of 8, 16 and 32 bits, capability list walks, bus master enabling
and MSI-X setup: `pci_msix_enable` sets the enable bit of the MSI-X
capability and `pci_msix_set_vector` maps the table BAR and writes into
one table entry the address and data of `platform_msi_compose`: the local
APIC address and the vector on the PC, the ITS doorbell and an event ID on
aarch64. The target CPU of each vector is chosen in turn among the CPUs
that have started (`pick_msi_cpu`), so the devices that kinit sets up
interrupt different CPUs. Devices set up during boot, before the
application processors start, interrupt the boot CPU.

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

Interrupts arrive through MSI-X. Each device gets one interrupt number
from `irq_alloc` (a vector from 40 upwards on x86_64, an LPI from 8192 on
aarch64), registered with `irq_register`. The configuration and every queue
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

## Partitions (P4)

`block/part.c` reads the GUID partition table of every disk once, from
`kinit`, before the root is mounted. It checks the signature, the CRC of
the header and of the entry array, and that the header names its own
sector, and it reads the backup header from the last sector when the
primary one fails these checks. Every used entry whose sectors lie in the
usable range becomes a `struct partition`, a block device named after the
disk and its entry number, as `vda1`, whose `rw` adds the first sector of
the partition and calls the driver of the disk. `blockdev.disk` names the
disk of a partition. `/dev/partitions` lists one line per partition,
`name disk partuuid typeuuid bytes`, with the GUIDs in lowercase. A
partition and its disk have separate buffers in the block cache, which
means a disk is not written as a whole while one of its partitions is
mounted.

A Limine executable file request reports the GPT disk and partition
GUIDs of the disk the kernel was loaded from (`bootinfo.boot_disk_guid`).
`mount_root` mounts the device that `root=` names, either as
`root=PARTUUID=GUID` or as a device name, or the initrd for
`root=initrd`. Without `root=`, it looks on the boot disk, or on `vda`
when the boot disk is not among the disks, for the partition whose type
is the root type of the Discoverable Partitions Specification for the
machine (`4f68bce3-e8cd-4db1-96e7-fbcaf984b709` on x86_64,
`b921b045-1df0-41c3-af44-4c6f280d3fae` on aarch64). A `vda` without a
partition table is mounted as a whole, as before P4, and the initrd is
the root when nothing mounts. `swap_attach` takes the swap partition
type (`0657fd6d-a4ab-43c4-84e5-0933c84b4f4f`) on the same disk, or the
whole of `vdb` when it has no partition table. `fsinit` accepts
`PARTUUID=GUID` as the device of an fstab entry and finds its name in
`/dev/partitions`.

Since P5 a program that wrote a new table asks for it to be read again
with the `ioctl` `BLKRRPART` on the device file of the disk, which only
root may use. `part_rescan` refuses with `EBUSY` while the root or the
swap device lies on the disk (`part_hold`). Otherwise it writes back the
buffers of the disk and reads its table again. The device of an entry
number remains registered after a rescan, which updates its position,
size and GUIDs, gives a partition whose entry is now empty the size 0, which also
removes it from `/dev/partitions`, and registers the entries that are
new. `devfs_set_size` gives the device files their new sizes. Mounting a
partition of the disk while its table changes is not prevented and is
the caller's mistake.

`tools/mkgpt` (`MKGPT`) writes GPT disk images on the host: a protective
MBR, both headers and arrays, partitions aligned to 1 MiB with the type
`bios`, `esp`, `swap`, `root-x86_64`, `root-aarch64`, `home` or `linux`,
an image copied into a partition and fixed or random GUIDs. The same
source runs on minios as `part`, and `mkfs`, `mkfat` and the `limine`
utility run there as well (package `disktools`, and `limine`). With the
size 0, each of the three formats an existing file or a device file such
as `/dev/vdc` at its current size without truncating it. `part -l` lists a
table. None of them holds the whole disk in memory: they write the
metadata and the copied files, and `part` asks the kernel to read the new
table.

The `disk_tools` case attaches swap as `vdb` and an empty disk as `vdc`.
`/etc/tests/disktools.sh` divides `vdc` with `part` into an EFI system
partition, a swap partition and a root partition, finds them in
`/dev/partitions`, formats the first with `mkfat` and the third with
`mkfs` from a small tree, mounts both, writes a file, and writes two other
tables, the second of which brings the first layout back. The post
script lists the table with the host `mkgpt`, reads the files with the
host `mkfat` and `mkfs`, and checks the mfs with `fsck`.

## Disk image and tests

`make disk` creates `build/disk.img`, a 512 MiB mfs image (`DISK_MB`
overrides the size). `make run` and `make gdb` attach it as
`virtio-blk-pci`. The test runner copies the image for every case so
cases start from identical contents and do not affect each other; a case
may ship its own `disk.img`. A case file `swap` adds a zero filled swap
device, `mfs2` an empty mfs image of the given size in MiB (`DISK2` in
the post script) and `fat` one FAT image per line (`<size_mb> <12|16|32>
[dir]`, built by `mkfat`; `FATIMG` and `FATIMGS` in the post script), in
that order after the root disk. An executable `mkdisk` writes the
case's disk instead, with `MKGPT`, `MKFAT` and `MKFS` in its environment.
On x86_64 the harness boots the CD first, since a GPT disk has a
protective MBR with a boot signature that the BIOS would try first.

The `gpt_boot` case boots from a disk that `mkdisk` writes with an EFI
system partition, a swap partition and the shared root image as the root
partition. The kernel must find the table, mount the root partition by
its type and use the swap partition, and `/etc/tests/gpt.sh` checks
`/dev/partitions`, mounts the EFI system partition by its PARTUUID and
two tmpfs instances through `fsinit`, exercises files, directories,
links, renames and a program run from `/tmp`, refuses a copy beyond a
size limit of 1 MiB and unmounts everything again.

The `blk` case checks the geometry reported by the device, raw single
sector, 128 sector and past the end transfers, delayed write back through
the cache, LRU eviction with more blocks than buffers, write back of an
evicted dirty buffer, and `/dev/vda` reads and writes across block
boundaries through the VFS followed by `sync`.
