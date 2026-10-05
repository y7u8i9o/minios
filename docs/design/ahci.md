# AHCI

R3 of `docs/plan/release-0.5.0.md` (D4 of `docs/plan/drivers.md`) adds a
driver for AHCI SATA controllers, `kernel/drivers/ahci.c`. ATA disks
become the block devices `sda`, `sdb` and so on. ATAPI CD drives become
`sr0`, `sr1` and so on through the SCSI module of the block layer
(`block.md`).

## Origin

The driver adapts `MdeModulePkg/Bus/Ata/AtaAtapiPassThru/AhciMode.c` and
`AhciMode.h` of edk2, revision `999fd0f12a27709eee04b93e46bd867e6b0163a5`.
The files are licensed under BSD-2-Clause-Patent. The source file retains
the copyright notices, and `third_party/edk2/` contains the licence text
and a README that names the adapted files.

The register definitions, the command list and FIS layouts, the FIS
construction, the port reset, the error recovery, the retry rule, the
command start and the port initialization follow edk2. The adaptation
differs from edk2 in these points:

- Each port has its own command list, received FIS area, command table
  and bounce pages. edk2 shares one command list between all ports. The
  ports of minios work in parallel.
- edk2 stops the command engine and FIS reception after every command.
  minios leaves the port running between commands and stops the engine
  only for the error recovery.
- edk2 polls. minios waits for the clearing of the command issue bit,
  which an MSI interrupt reports, and polls without an interrupt.
- minios takes the controller from the firmware through the BIOS/OS
  handoff of AHCI 1.3 when the controller offers it. edk2 is the firmware
  and has no such step.
- SMART, device sleep, power up in standby, staggered spin up, the
  transfer mode of SET FEATURES and the IDE controller init protocol of
  edk2 are left out.

## Probe

`ahci_init` runs in `kinit` after `nvme_init` and before the partition
scan. It starts every PCI function of class 01, subclass 06, interface
01, up to four controllers. For each controller:

1. BAR 5 (ABAR) is mapped, and the BIOS/OS handoff takes the controller
   from the firmware.
2. GHC.AE is set, GHC.HR resets the HBA, and GHC.AE is set again.
3. The interrupt is MSI-X, else MSI, else none. The kernel option
   `ahci=poll` selects polling, which the QEMU controller cannot otherwise
   show.
4. Each implemented port is probed. The command engine and FIS reception
   stop. The port receives a page with the command list at offset 0 and
   the received FIS area at 0x400, and a page for the command table.
   Power on and spin up are requested, aggressive link power management
   is disabled, and FIS reception starts.
5. The phy must report a device within 15 ms. The device must clear BSY,
   DRQ and ERR within 5 seconds, and the first D2H register FIS must set
   the signature. The signature distinguishes ATA disks from ATAPI drives.
   A port multiplier is not supported.
6. Only a port with a device receives its 32 bounce pages, and its
   interrupts are enabled. IDENTIFY DEVICE or IDENTIFY PACKET DEVICE gives
   the model, the serial number and the firmware revision.

A controller without 64 bit addressing receives no page above 4 GiB.

## Commands

Every command uses slot 0 of its port. `build_command` fills the command
header, the command FIS, the ATAPI command and one PRD entry per bounce
page. `start_command` clears the port status, enables FIS reception,
wakes the link, clears a busy task file through command list override
when the controller offers it, starts the engine and sets bit 0 of PxCI.

`wait_command` waits until bit 0 of PxCI clears or PxIS reports an
error. The interrupt handler moves the bits of PxIS into the
`irq_status` of the port, clears PxIS and the HBA interrupt status, and
wakes the waiters. The waiter reads the port itself on every pass: after
each interrupt, every second with an interrupt, and every millisecond
without one. A completed command with ERR in the task file is a device
error.

After an error, `recover_port_error` stops the engine, resets a device
that remains busy, and clears the status. The engine starts again with
the next command. A command is repeated up to three times after a CRC
error between memory and the controller or between the controller and
the device, as `AhciShouldCmdBeRetried` of edk2 decides. A command
without completion within the timeout (5 seconds for ATA, 20 seconds for
ATAPI) is not repeated.

## ATA disks

A disk needs LBA addressing. LBA48 disks use READ DMA EXT, WRITE DMA EXT
and FLUSH CACHE EXT. Other disks use READ DMA, WRITE DMA and FLUSH CACHE
with at most 256 sectors per command. The logical sector size comes from
words 106, 117 and 118 of the IDENTIFY data and may be at most 4 KiB.
Data passes through the bounce pages, at most 128 KiB per command.

## ATAPI drives

The ATAPI transport sends the PACKET command with PIO data, as edk2
does, and lets the device choose the byte count of each data block. A
device error with ERR in the task file is a CHECK CONDITION, and the SCSI
module then asks REQUEST SENSE. The SCSI module registers the drive and
follows its medium (`block.md`).

## /dev/devices

`ahci_describe` adds the node `ahci` with one node per controller, and
below it one node per port with a device: the block device, the kind,
the model, the serial number and the firmware revision.

## Locks

`ahci_port.lock` (mutex) serializes the commands of a port and protects
its DMA memory. `ahci.lock` (spinlock) protects the interrupt status of
the ports and is the condition lock of the controller's wait queue.
`locking.md` gives the order.

## Tests

The harness file `diskif` with the word `ahci` attaches the root disk to
port 0 of the q35 controller, or to an `ich9-ahci` controller on aarch64.
The harness file `cd` attaches further CD drives: `empty` for a drive
without a medium, `iso DIR` for an image that xorriso builds.

- `ahci` runs the `blk` checks on `sda` with MSI.
- `ahci_polled` runs them with `ahci=poll`.
- `ahci_aarch64` runs them on aarch64.
- `ahci_cd` runs the kernel test `cdrom` against the boot CD of the q35
  machine, `sr0`, and an empty drive, `sr1`. It reads the volume
  descriptors, 32 sectors at once, the last sector and the device file
  up to its end, checks that a read past the end and a write fail, and
  checks that the empty drive has no sectors and reports `ENOMEDIUM`.
