# The installer

The release of minios is an installation medium rather than a system that
boots as it is (P7 of `docs/plan/packaging.md`). The medium boots into a
small installer environment, which partitions a disk, installs a chosen
group of packages onto it from the signed repository of the medium, and
configures the installed system, which then boots from that disk by
itself.

## The medium

`tools/mkinstaller.sh` (`make installer`, `build/installer.img`) builds a
GPT disk image, the form that is written to a USB stick and that QEMU
attaches as a virtio disk. It has these partitions.

| Partition | Content |
|---|---|
| BIOS boot, 1 MiB, x86_64 only | the BIOS stage of Limine, written by `limine bios-install` |
| EFI system, FAT32 | `EFI/BOOT/BOOTX64.EFI` or `BOOTAA64.EFI`, `limine/limine-bios.sys`, `minios/kernel.elf`, `minios/initrd.tar` and `limine.conf` with the command line `root=initrd swap=off` |
| linux, mfs | `repo/ARCH/`, the repository of every package except `tests`, signed with the key of the build, and optionally `installer.conf` |

The installer environment is the initrd. `mkinstaller.sh` installs the
groups and packages `minimal`, `disktools` and `installer` into a tree with
the host build of `pkg`, from the repository it has just written and with
the public key of the build, and replaces `/etc/init.conf` and
`/etc/fstab` of that tree. Its init runs `fsinit`, which mounts tmpfs
instances on `/tmp` and `/run`, sets the keyboard layout and starts
`/usr/bin/installer` as the console program. `swap=off` makes the kernel
leave every disk alone, since the target disk has no table yet and would
otherwise become swap. The kernel finds no root partition on the medium
and runs from the initrd.

## The installer program

`user/installer/` holds the back end (`backend.c`) and the text front end
(`text.c`) with their shared declarations (`installer.h`). The installer
first finds the medium: the partition of the type linux that holds an mfs
with `repo/MACHINE/index`, which it mounts on `/run/installer/medium` and
whose disk it excludes from the targets. When the medium holds
`installer.conf`, or `-a FILE` names an answer file, it installs without
questions and powers off afterwards, also after a failure, since init
would start it again with the same answers. Otherwise it lists the disks
with their sizes and asks for the disk, the package group, further
packages, the language, the keyboard layout, the time zone, the size of
swap, the password of root, the first account with its full name and
password, shows the disk to be erased and installs after the word `yes`.

The answer file holds `key value` lines: `disk`, `group`
(`desktop-system` by default), `packages`, `lang` (`en_US.UTF-8`),
`keymap` (`us`), `timezone` (`UTC`), `swap_mb` (256), `root_password`,
`user`, `user_fullname`, `user_password`, `reuse_home`, `cmdline` and
`poweroff` (`yes`). The installer refuses a disk that does not exist, is
the medium or is too small, a missing password of root, an invalid name
of the account, and a keyboard layout or time zone that the environment
does not have.

The installation runs these steps, each of them a program an
administrator would run:

1. `part` writes a GPT with, on x86_64, a BIOS boot partition of 1 MiB,
   an EFI system partition of 256 MiB, the swap partition and the root
   partition of the machine over the rest, and the kernel reads the new
   table through `BLKRRPART`. The installer finds the partitions and
   their GUIDs in `/dev/partitions` by their types.
2. `mkfat -t 32` formats the EFI system partition and `mkfs` the root,
   which are mounted on `/run/installer/target` and
   `/run/installer/target/boot`.
3. `/etc/kernel/cmdline` of the target receives `root=PARTUUID` of the
   root partition and the `cmdline` of the answers, and on x86_64
   `/etc/kernel/bios-disk` receives the `PARTUUID` of the BIOS boot
   partition, which `pkg` resolves through `/dev/partitions` when the
   boot loader is upgraded on the installed system.
4. `pkg --root /run/installer/target --config /run/installer/pkg.conf
   --keys /etc/pkg/keys` updates the index of the repository of the
   medium, a `file://` repository, and installs the group and the
   further packages, resolving their dependencies through the index and
   using the archives where they are. The installation of the kernel
   package writes `/boot/limine.conf` from the command line.
5. The installer rewrites `/etc/fstab` of the target without the entry of
   the data volume of the build image, with `/boot` as the EFI system
   partition by its `PARTUUID`, and with `/home` on the volume of
   `reuse_home` when that is given. Without it, the homes are seeded from
   `/usr/share/skel/home` as `fsinit` seeds a new data volume, `/root`
   from `/etc/skel`, the password of root is set, and the first account
   replaces the account `user` of the seed with uid 1000, joins `wheel`
   and receives its home. The account databases are the files below
   `/home/.local/etc`, to which `/etc/passwd`, `/etc/group` and
   `/etc/shadow` lead (`users.md`).
6. `lang` and `keymap` go into `/etc/desktop.conf`, and `/etc/localtime`
   becomes a link to the zone. `pkg bootconfig` writes the boot loader
   configuration again, and on x86_64 `limine bios-install` writes the
   BIOS stage to the disk.
7. The log of the installer, `/run/installer/installer.log`, is copied to
   `/var/log/installer.log` of the target, and both partitions are
   unmounted.

minios has no host name, and the installer therefore does not ask for
one.

## Changes elsewhere

- `mkdir` reports `EEXIST` for an existing name before any other error,
  as POSIX and Linux do, which lets `pkg` create the directories of
  `/run/installer/target/var/lib/pkg` across the read only initrd.
- `init` starts `login` when the console program of `init.conf` is not
  installed, which is the case for the greeter on a system without the
  desktop (`init.md`).
- The kernel takes `swap=off` (`swap.md`).
- `pkg` reads `file://` repositories and takes `--keys DIR`
  (`packages.md`).

## Tests

The cases `install_auto` and `install_console` boot an installation
medium that `mkdisk` writes with `mkinstaller.sh` and the answer file of
the case, with an empty second disk (`mfs2`). The case file `diskboot`
boots the medium without a CD. The installer installs and powers off,
which ends the first boot, and `boot2` with the word `disk2` boots the
second disk alone, through the BIOS stage on x86_64 and through UEFI on
aarch64. `install_auto` installs `desktop-system` with the account `anna`,
the French keyboard layout and the zone `Europe/Berlin`, and its second
boot must reach the greeter, after which `stop2` ends it. Its post script
reads the installed disk on the host and checks the account, its
membership of `wheel`, the password hashes, `desktop.conf`, the log of the
installer, the boot loader configuration and the file system with
`fsck`. `install_console` installs `standard` with the account `user` and
must reach `minios login:` through the fallback of init.
