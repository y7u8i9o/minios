# Persistent storage

The root filesystem is an mfs image that the build regenerates whenever
a program changes, and files written during a session are lost at the
next build. Since P3 of `docs/plan/packaging.md` the build assembles the
image by installing the packages of the base system (`packages.md`). Since 2026-09-06 the home directory
lives on a data volume that the build creates once and never rebuilds,
and the user's configuration is stored in that home directory.

## The data volume

`data.img` in the repository root (ignored by git, `DATA` and `DATA_MB`
in the Makefile, 256 MiB by default) is an mfs image created empty by
`mkfs` when it does not exist. `make clean` leaves it alone; `make
clean-data` removes it. `tools/run.sh` attaches it as the third virtio-blk
device, `vdc`, after the root image and the swap image; `--data FILE` or
`DATA` selects another file and `--no-data` boots without one.

## fstab and fsinit

`/etc/fstab` lists the filesystems mounted at boot, one per line: device,
mount point, type and options (`fstab(5)`). The shipped table has one
entry:

    vdc /home mfs nofail,seed=/usr/share/skel/home,homes

`fsinit` (`user/coreutils/fsinit.c`) reads the table and mounts every
entry whose mount point is not listed in `/dev/mounts` yet. `nofail`
makes a device that cannot be mounted a skipped entry rather than an
error, which is how a boot without the data volume, and every boot test,
proceeds with the home directory of the root image. `seed=DIR` copies the
tree below `DIR` into the mount point when the mounted filesystem is
empty: the first boot with a fresh volume fills `/home` with the
skeleton, later boots find it populated and leave it alone. `noauto`
skips an entry. `homes` (U3 of the multiuser plan, `users.md`) converts a
volume of the single user layout and makes the missing homes of the
accounts. The exit status is 1 when a required mount fails.

`init` runs `/bin/fsinit` and waits for it before the first shell; a
failure is reported on the console and the system continues from the
root image. `fsinit` may run again at any time, since mounted entries are
skipped.

The skeleton was `user/home/`, installed twice by `user/Makefile`: on the
root image as `/home`, for a boot without a data volume, and as
`/usr/share/skel/home`, the seed. Since U3 it is `user/skel/`, installed
as `/etc/skel` and copied into the homes of the accounts, and the seed
contains `.local` with the account databases (`users.md`). `mkfs` used to skip every name that
begins with a dot, so `.shrc` never reached an image; it now skips only
`.`, `..` and `.DS_Store`.

## User configuration

`/etc/desktop.conf` is the shipped default configuration; the user's
choices are written to `$HOME/.config/desktop.conf`, on the data volume.
`conf_read_path` and `conf_write_path` in `minios/conf.h` (`lib/libc/src/conf.c`)
name the file to read, the user's when it exists and the default
otherwise, and the file to write, creating `$HOME/.config`. The settings
program, the desktop client, the terminal, X12's keymap reload and the
toolkit theme use them.

## System state

The DHCP client retains its lease in `/usr/local/state/dhcpc/IF.lease`,
`/home/.local/state/dhcpc` on the data volume,
(N16, `docs/design/network.md`), since the home volume is the only
storage that survives a build, and `fsinit` mounts it before init starts
the `dhcp` service.

## mount

`mount` without arguments prints the mounted filesystems from
`/dev/mounts`, one `path type name` line each.

## Tests

`tests/cases/persist` boots with an empty mfs image as `vdb` and runs
`/etc/tests/persist.sh`: a table with a seeded entry and a `nofail` entry
for a missing device mounts and seeds the volume, the listing of `mount`
shows it, a file is written and a seeded file removed, the volume is
unmounted and mounted again without being seeded a second time, a third
run finds it mounted already, and a table with a required missing device,
an invalid entry and a missing table each fail with status 1. After the
guest exits, the post script reads the written file and a seeded file
from the image with `mkfs --cat`, which shows that the data reached the
disk.
