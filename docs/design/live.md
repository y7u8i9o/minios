# The live medium

R6 of `docs/plan/release-0.5.0.md` adds the live medium. The live medium is
one hybrid ISO image per architecture. The image boots from a CD drive and
from a disk, such as a USB stick to which the image is written. The image
runs the desktop from the medium and offers the graphical installer. The
release publishes the image beside the installation medium of
`installer.md`.

## Build

`make live` runs `tools/mklive.sh ARCH BASE APPS OUT [ANSWERS]` and writes
`build/minios-live-VERSION-ARCH.iso`. The script takes the tools from the
environment that the top level Makefile exports: `PKGHOST`, `PKGSIGN`,
`PKG_KEY_FILE`, `PKG_PUB`, `KERNEL`, `LIMINE` and `XORRISO`. `CMDLINE`
adds words to the kernel command line, which the boot tests use for the
name of the test. The script runs these steps.

1. `mkrepo.sh` signs a repository at `/repo/ARCH` of the tree. The
   repository contains the base packages except `tests` and the
   applications of the current version, as on the installation medium.
2. The host build of `pkg` installs the groups `desktop-system`,
   `installer` and `disktools` from that repository into the tree. The
   script removes the cache of the repository index afterwards.
3. `pkg perms` lists the modes and owners of the installed files. The
   script applies the modes to the tree. xorriso receives the owners other
   than root as `-chown` and `-chgrp` commands after `-chown_r 0 /`.
4. The script writes the live configuration (below).
5. The script copies the kernel to `/boot/minios/kernel.elf`, the Limine
   stages to `/boot/limine` and the EFI applications to `/EFI/BOOT`. The
   boot entry `minios VERSION live` has the command line
   `root=LABEL=MINIOS_LIVE swap=off` and the resolution 1024x768.
6. `xorriso -as mkisofs -R -V MINIOS_LIVE` writes the image with Rock
   Ridge, El Torito entries for BIOS and UEFI, an EFI system partition
   image and a protective MBR. `limine bios-install` writes the BIOS stage
   into the image. The image of version 0.4.0 has a size of 50 MiB.

`make release` builds the live medium with the installation medium.
`tools/release.sh` copies it as `minios-VERSION-ARCH-live.iso` into the
results.

## Boot

Limine loads the kernel from the ISO 9660 file system. The kernel finds
the root through `root=LABEL=MINIOS_LIVE` (`iso9660.md`). The search
covers CD drives on AHCI and USB and disks on every controller. A medium
in a CD drive is `srN`, and an image written to a USB stick is `sdX`.
`swap=off` prevents the kernel from using a swap partition of a disk of
the machine, because the live system must not change the disks of the
machine.

The root is read only. `/etc/fstab` of the tree mounts tmpfs instances on
`/home`, `/root`, `/tmp`, `/run`, `/var/log`, `/var/db` and `/var/run`.
`fsinit` seeds `/home` from `/usr/share/live/home` with the option
`homes`, and `/root` from `/etc/skel`. The seed of `/home` is the skeleton
of the image with the account `live` in place of the account `user`.
`live` has uid 1000, no password and membership of `wheel`. The account
databases below `/home/.local/etc` are therefore writable for the duration
of the session.

## Session

The init configuration of the tree starts `fsinit`, the key map, the
audio service, the network and the DHCP client, and runs
`greeter -a live` as the console program. The option `-a NAME` of the
greeter starts the session of the account `NAME` once without the login
window (`users.md`). The login window follows when that session ends.

The launcher menu of the panel lists the entry "Install minios", which
the script appends to `/etc/launcher`. The entry runs
`doas /usr/bin/installer-gui --session`. The rule
`permit nopass live as root cmd /usr/bin/installer-gui args --session`
at the end of `/etc/doas.conf` lets `live` start the installer without a
password. doas applies the last matching rule. The rule therefore takes
precedence over `permit :wheel` for this command alone.

## Installer

`installer-gui --session` opens its window in the running session. The
program does not start X12 and does not fall back to the text installer.
After an installation the page offers Restart in place of Power off.
Closing the window ends the program and returns to the session.

The installer recognizes the live medium by `/etc/live-medium`, which
contains the volume identifier, and by the repository
`/repo/MACHINE/index` below the root. The repository of the medium is
then `file:///repo/$arch`, and the answer file is `/installer.conf`. The
installer reads the volume descriptor of every disk. A disk whose ISO
9660 volume identifier equals the one of `/etc/live-medium` is the medium
and is excluded from the targets. A CD drive is never a target, because
the target list contains only disks: `vdX`, `sdX` and `nvmeCnN`. Disks of
size 0 are omitted.

The installer installs packages from the repository. The installed system
therefore contains none of the live configuration: no account `live`, no
tmpfs entries and no launcher entry.

When `mklive.sh` receives an answer file, the file becomes `/installer.conf`
of the tree. The init configuration then runs `installer` as the console
program in place of the greeter. The text installer installs without
questions and powers off. A task would not work here: init serves
`initctl` requests only after a task has ended, and the installer calls
`initctl poweroff` before it ends.

## Changes elsewhere

- `minios/disk.h` of libc provides `disk_partition_index`, which returns
  the GPT index of a partition name such as `nvme0n1p1` or `vda1`. The
  installer and `pkg` used the partition name without the disk name, which
  gave `p1` for an NVMe disk and made `limine bios-install` fail.
- The installer accepts the disk names of NVMe and SATA disks
  (`inst_is_disk_name`).
- The greeter takes `-a NAME`, and `greeter(1)` describes the option.

## Tests

The harness file `bootiso` is an executable that writes the boot image of
a case in place of `tools/mkiso.sh`. The harness file `bootcd` with the
word `usb` attaches the boot image as a USB CD drive on its own xHCI
controller.

- `live` (x86_64) boots the live medium as a CD on the AHCI controller of
  q35. The test `live_session` starts init and waits for the panel and the
  desktop of uid 1000. It then checks with `doas -C` as `live` that the
  rule permits the installer without a password, and it checks the entry
  of the launcher menu.
- `live_install` (x86_64) boots a live medium with an answer file and an
  empty NVMe disk of 2 GiB. The installer installs onto `nvme0n1` and
  powers off. The post script checks the account of the answer file, the
  absence of the account `live`, the log of the installer, the boot loader
  configuration and the file system. The second boot starts the installed
  disk from the NVMe controller and waits for the greeter.
- `live_usb` (aarch64) boots the live medium as a USB CD drive and runs
  `live_session`.

## Limits

- Changes in the live session are lost at shutdown. The medium has no
  persistent storage.
- The live system uses no swap.
- Every file written in the session occupies memory of the machine.
