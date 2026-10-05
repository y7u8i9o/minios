# NVMe

R2 of `docs/plan/release-0.5.0.md` (D3 of `docs/plan/drivers.md`) adds a
driver for NVMe controllers, `kernel/drivers/nvme.c`. It is written from
the NVM Express Base Specification 2.0 and the NVM Command Set
Specification 1.0.

## Probe

`nvme_init` runs in `kinit` before the partition scan, because the probe
sleeps while it waits for admin commands. It starts every PCI function of
class 01, subclass 08, interface 02, up to four controllers. For each
controller the driver performs these steps:

1. It maps BAR 0 and reads CAP. The controller must support the NVM
   command set, pages of 4 KiB and queues of 64 entries.
2. It clears CC.EN and waits for CSTS.RDY to clear, for at most the time
   of CAP.TO.
3. It allocates the admin queue pair, 32 bounce pages and a PRP list
   page, and writes AQA, ASQ and ACQ.
4. It sets up the interrupt: MSI-X entry 0, else MSI, else none. Without
   an interrupt, INTMS masks every vector.
5. It sets CC.EN with entries of 64 bytes for submission queues and 16
   bytes for completion queues, and waits for CSTS.RDY.
6. Identify Controller gives the model, the serial number, the firmware
   revision, MDTS, the number of namespaces and the volatile write cache
   flag. MDTS limits the transfer size below the 128 KiB of the bounce
   pages.
7. Set Features (Number of Queues) asks for one I/O queue pair. Create
   I/O Completion Queue and Create I/O Submission Queue create queue 1
   with 64 entries on interrupt vector 0.
8. Identify with CNS 2 lists the active namespaces on controllers of
   version 1.1 and later. Older controllers use the identifiers 1 to NN.
   Identify Namespace gives the size and the LBA format of each
   namespace. A namespace with metadata or with blocks larger than 4 KiB
   is skipped.

Each namespace becomes the block device `nvmeCnN`, where C is the index
of the controller and N the namespace identifier. The partitions of such
a disk are named `nvme0n1p1` and so on (`block.md`).

## Commands

One command is outstanding per controller at a time. `submit` writes
the command into the submission queue, rings the tail doorbell and
waits for the completion with the same command identifier. The waiter
reads the completion queue itself on every pass: after each interrupt,
every second with an interrupt as a safeguard, and every millisecond
without one. The interrupt handler reads both completion queues and
wakes the waiter. A completion queue entry belongs to the driver when its
phase tag equals the phase of the queue, which toggles at each wrap. The
head doorbell is written after the entries are consumed.

A command that does not complete within 5 seconds (admin) or 30 seconds
(I/O) marks the controller failed. Its identifier could be reused, so
every later command fails with `EIO`. A completion with a nonzero status
code returns `EIO` and logs the status code type and the status code.

`nvme_rw` splits a transfer into pieces of at most the bounce size.
Data passes through the bounce pages. The first page is PRP1. The second
page is PRP2 for a transfer of two pages, and the PRP list page
describes the pages after the first for a longer transfer. `nvme_flush`
sends FLUSH when the controller reports a volatile write cache.

The kernel option `nvme=poll` disables the interrupt. The QEMU `nvme`
device always offers MSI-X, and the option lets the boot tests reach the
polling path, which a controller without MSI-X and MSI uses.

## Locks

`nvme.io_lock` (mutex) serializes the commands of a controller: the
submission tails, the command identifiers, the bounce pages and the PRP
list. `nvme.lock` (spinlock) protects the completion heads and phases and
the completion record, and it is the condition lock of the wait queue.
The I/O queue is published under `nvme.lock` once it is initialized,
since the interrupt handler reads it. `locking.md` gives the order.

## /dev/devices

`nvme_describe` adds the node `nvme` with one node per controller: the PCI
address, the version, the model, the serial number, the firmware
revision, the interrupt, the largest transfer, the write cache and the
namespaces.

## Root disk

Without `root=`, `mount_root` tries the boot disk, then `vda`, then the
first registered disk that is not a partition and not a CD drive
(`BLOCKDEV_CDROM`). A machine whose only disk is NVMe therefore mounts its
root partition, or the whole disk without a partition table.

## Tests

The harness file `diskif` selects the controller of the root disk:
`nvme` attaches it as a QEMU `nvme` device. The `blk` kernel test takes
the device from `blkdev=`.

- `nvme` runs the `blk` checks on `nvme0n1` with MSI-X.
- `nvme_polled` runs them with `nvme=poll`.
- `nvme_root` boots without `root=` and mounts the root from `nvme0n1`.
