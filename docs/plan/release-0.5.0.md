# Release 0.5.0: more machines, live medium

This plan contains the milestones of release 0.5.0 of
`docs/plan/roadmap.md`. Milestone identifiers use the prefix `R`. Each
milestone ends with boot tests, extends or adds the design documents
named in it, and is marked completed here when its boot tests pass.
R2, R3 and R4 are the milestones D3, D4 and D5 of `drivers.md`, which are
marked completed there as well.

## 1. Motivation and scope

minios boots from virtio disks only. The default QEMU machines without
virtio devices attach disks as NVMe or SATA devices, and CD images as
ATAPI drives on the AHCI controller of q35 or as USB drives. UTM attaches
the CD image of an aarch64 machine as a USB drive. The release medium is
a GPT disk image, which a CD drive cannot boot, and it boots into the
installer without a usable system.

After this plan minios reads NVMe disks, SATA disks, ATAPI CD drives, USB
disks and USB CD drives, also behind USB hubs. The release adds a live
medium: one ISO image that boots from a CD drive and from a disk, runs
the desktop from the medium and offers the installer. The installer reads
the packages from the medium. The kernel starts scripts that begin with
`#!`.

## 2. Fixed decisions

- The drivers follow the fixed decisions of `drivers.md`: generic code
  built on both architectures, MSI-X, else MSI, else polling, and no code
  from Linux.
- Block devices are named as on Linux: `nvme0n1` for a namespace, `sda`
  and further letters for SATA and USB disks, `sr0` and further numbers
  for CD drives. A partition of a disk whose name ends in a digit has a
  `p` before its number, as `nvme0n1p1`.
- ATAPI and USB mass storage both carry SCSI commands. One module of the
  block layer (`block/scsi.c`) builds the commands and registers disks and
  CD drives. A transport passes a command block, a data buffer and a
  direction.
- CD drives have 2048 byte sectors. The block cache retains its 4 KiB
  blocks, of two sectors each.
- The ISO 9660 file system is read only. It adapts the reader of Limine
  (BSD 2-clause licence, `common/fs/iso9660.s2.c` of Limine 10.8.5), whose
  notice the source file retains. It reads the Rock Ridge extensions
  (IEEE P1282, SUSP), because the names, modes, owners, symbolic links
  and times of a minios tree need them. Joliet is not read.
- The live medium is a hybrid ISO image built by xorriso with Limine. Its
  ISO 9660 file system is the root of the live system, mounted read only.
  tmpfs instances provide the writable directories.

## 3. Milestones

### R1. `#!` scripts (completed 2026-10-05)

`execve` of a file whose first two bytes are `#!` runs the interpreter
that the first line names. The rest of the line after the interpreter,
without the surrounding blanks, is one optional argument. The new argument
vector is the interpreter, the optional argument, the path of the script
and the arguments after the first of the original vector. The line is at
most 255 bytes. An interpreter may itself be a script, up to four levels.
The set user id and set group id bits of a script have no effect. A
script without execute permission fails with `EACCES`, and a missing
interpreter fails with `ENOENT`. libc retains its fallback to `/bin/sh`
for files without `#!` (`ENOEXEC`).

Boot test: `shebang` runs scripts through `execve` directly: an argument
on the `#!` line, arguments after the script, a nested interpreter, a too
deep nesting, a missing interpreter, a line without a newline, a script
without execute permission and a set user id script. A carriage return
at the end of the line is removed, which lets scripts with DOS line ends
run.

Document: `docs/design/process.md`.

### R2. NVMe (D3)

A driver for NVMe controllers (PCI class 0108 interface 02): controller
reset and enable, the admin queue, identify of the controller and of the
active namespaces, one I/O queue pair, and the commands READ, WRITE and
FLUSH with PRP lists. Each namespace becomes the block device `nvmeCnN`.
Completions arrive through MSI-X, MSI or polling.

Boot tests: `nvme` boots with the root disk on a QEMU `nvme` device and
no virtio disk and runs the `blk` checks against it. `nvme_msi` and
`nvme_polled` repeat the boot without MSI-X and without any interrupt.
The harness file `diskif` selects the controller of the root disk.

Document: `docs/design/nvme.md`.

### R3. AHCI with CD drives (D4)

A driver for AHCI controllers (PCI class 0106 interface 01): the HBA
reset, the ports with a device, the command list and the received FIS
area of each port, and one command at a time per port. ATA disks use
IDENTIFY DEVICE, READ DMA EXT, WRITE DMA EXT and FLUSH CACHE EXT and
become `sdX`. ATAPI drives use the PACKET command with the SCSI commands
of `block/scsi.c` and become `srN`. A CD drive reports a missing medium
as `ENOMEDIUM` and reads the capacity again after a medium change.

Boot tests: `ahci` boots with the root disk on the SATA controller of q35
and runs the `blk` checks. `ahci_cd` reads the volume descriptors and the
last sector of an ISO image in a CD drive and checks the capacity, and
checks a drive without a medium. `ahci_aarch64` runs the root disk test
on aarch64 with an `ich9-ahci` controller on PCI.

Document: `docs/design/ahci.md`, with `block/scsi.c` in
`docs/design/block.md`.

### R4. USB mass storage and hubs (D5)

The xHCI driver gains bulk endpoints. The mass storage driver binds to
interfaces of class 08, subclass 06 (SCSI) and protocol 50 (bulk only).
It implements the command and status wrappers, the reset recovery of
the bulk-only transport and GET MAX LUN, and registers each logical unit
through `block/scsi.c` as a disk or a CD drive. The hub driver binds to
hubs (class 09). It reads the hub descriptor, powers the ports, resets a
port with a device, enumerates the device with its route string and
parent port, and reports disconnections. A device that is removed returns
`ENODEV` for every later request.

Boot tests: `usb_storage` boots with a `usb-storage` disk as the root and
no virtio disk and runs the `blk` checks. `usb_cd` reads an ISO image in
a USB CD drive. `usb_hub` attaches a keyboard and a disk behind a
`usb-hub` and checks both.

Document: `docs/design/usb.md`.

### R5. ISO 9660

A read only file system `iso9660`: the primary volume descriptor,
directories with multi-extent files, and the Rock Ridge entries `PX`,
`PN`, `SL`, `NM`, `TF`, `CE`, `RE` and `CL` with `ST` and `ER` from SUSP.
Inode numbers are the byte positions of the directory records. Without
Rock Ridge, names are lowercase and lose their version suffix, and modes
are 0555 for directories and 0444 for files. `mount -t iso9660 DEVICE
DIR` mounts a device. `root=LABEL=ID` mounts as the root the first block
device that contains an ISO 9660 file system with the volume identifier
`ID`, CD drives first, and waits up to ten seconds for USB devices.

Boot tests: `iso9660` mounts an image with Rock Ridge from a CD drive and
compares a tree of names, long names, modes, owners, symbolic links,
times, a large file and a deep directory with the host tree. `iso9660_plain`
mounts an image without Rock Ridge. `iso9660_root` boots with
`root=LABEL=` from a CD drive.

Document: `docs/design/iso9660.md`.

### R6. The live medium

`make live` builds `build/minios-live-VERSION-ARCH.iso` with
`tools/mklive.sh`. The image contains Limine, the kernel and the live
tree: the group `desktop-system`, the installer and the signed repository
of the release at `/repo/ARCH`. The kernel command line is
`root=LABEL=MINIOS_LIVE swap=off live`. The init of the live tree mounts
tmpfs instances on the writable directories and seeds `/home` from the
skeleton. The account `live` has no password and belongs to `wheel`. The
greeter takes `-a NAME`, which starts the session of that account without
a login, and the live init uses it. The desktop of the live session shows
the launcher "Install minios", which starts the graphical installer
through `doas`. The installer finds the repository below the root of a
live medium, excludes the disk of the medium from the targets, and
returns to the session after an installation. `make release` builds the
live medium as well.

Boot tests: `live` boots the image as a CD on the AHCI controller of q35
and reaches the desktop of `live`. `live_install` installs onto an NVMe
disk with an answer file from the live session and boots the installed
disk. `live_usb` boots the image on aarch64 as a USB CD drive and reaches
the desktop.

Document: `docs/design/live.md`, with changes to
`docs/design/installer.md`.
