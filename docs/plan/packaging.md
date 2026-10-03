# The base system as packages and the installer

This plan turns the whole system into packages that `pkg` installs into
the root filesystem, and turns the release image into an installer that
partitions a disk and installs a chosen set of packages onto it, in the
manner of current operating systems. Milestone identifiers use the
prefix `P` in order not to renumber the development milestones. Each
milestone ends with a boot test, extends the design documents named in
it, and is marked completed here when its boot tests pass. The work is
on the branch `bleeding-edge-packaging`.

## 1. Motivation and scope

The root filesystem is a copy of `build/initrd_root` that the build
writes to `build/disk.img` with `mkfs` after every change. Only the
fourteen bundled applications are packages, and they install under
`/usr/local`, a symbolic link to the data volume, because the data
volume is the only storage that survives a build. The base system is not
a package, `pkg` refuses a library that exists in `/lib`, and the release
ships an ISO that boots the system directly.

The kernel mounts whole devices only. Root is `vda` or the initrd, swap
is the whole of `vdb`, and there is no partition table code and no
tmpfs. `mkfs` and `mkfat` exist only as host tools, and `pkg` has no host
build. `pkg` writes every file in place without a temporary file, has no
notion of configuration files, and ignores the owner fields of its
archives. `lua` links libgui and libaudio because its gui, audio and MIME
bindings are compiled into the interpreter. Every boot case clones
`build/disk.img`, and the cases `pkg`, `pkg_repo` and `pkg_apps` expect an
empty package database.

After this plan every file of the system belongs to a package. The
release is a disk image holding the installer environment and a signed
repository. The installer writes a GPT disk with an EFI system partition,
a swap partition and a root partition, installs the package groups the
user selects, creates the accounts and installs the boot loader. An
installed system updates itself, its kernel included, through `pkg`.
Development runs boot an installed development disk that receives the
build's packages as offline updates, and boot tests boot images that the
host assembles from the same packages. A graphical front end of the
installer comes last.

## 2. Fixed decisions

- The layout is a merged `/usr`. Programs are in `/usr/bin`, libraries
  in `/usr/lib`, codec modules in `/usr/lib/codecs` and fonts in
  `/usr/share/fonts`. `/bin` and `/lib` are symbolic links into `/usr`,
  which means `/lib/ld.so` as the program interpreter and `#!/bin/sh` in
  scripts still resolve. `/usr/local` returns to its usual meaning,
  software that `pkg` does not manage, and `ld.so` searches `/usr/lib`
  before `/usr/local/lib`.
- Package format 2 stores paths relative to the installation root, as
  in `files/usr/bin/calc`. The manifest carries `format 2`, and `pkg`
  refuses an archive of format 1 with a message that it must be rebuilt.
- The records are in `/var/lib/pkg/NAME/`. Each line of the file record
  holds the path, mode, uid, gid, size and SHA-256 of one file.
- The owner and mode fields of the ustar headers are applied. Setuid and
  setgid bits are applied only to files owned by root.
- Installation writes every member to a temporary file in its target
  directory, renames all of them only after the whole transaction has
  been extracted, and writes the records last. A failure removes the
  temporary files and leaves the old files untouched. A running program
  that maps `libc.so` is not affected by its replacement, since mfs frees
  an unlinked file only when its last mapping goes away.
- The manifest key `config PATH` marks a configuration file, with the
  rules of pacman. An unmodified file is replaced on upgrade. A modified
  file is left in place and the new version is written beside it as
  `PATH.pkgnew`. A modified file is saved as `PATH.pkgsave` on removal.
  `/etc/passwd`, `/etc/group`, `/etc/shadow`, `/etc/fstab` and
  `/etc/profile` are configuration files of the package `base-files`.
- The provider of a library is the package whose `provides` line names
  it, and that line gives its ABI number. `/lib/abi` is removed. The rule
  that a package library may not exist in `/lib` becomes the rule that no
  two packages may provide the same library.
- `pkg` runs no scripts from packages. Its built-in triggers rewrite the
  launcher and MIME tables, as now, and in addition the boot loader
  configuration (P6).
- The packages and their groups are the following. A group is a
  metapackage without files, holding only `depends` lines. Manual pages
  ship in the package of their program.

| Group | Packages |
|---|---|
| minimal | `kernel`, `limine`, `libc`, `init`, `sh`, `coreutils`, `base-files`, `pkg`, `accounts` (login, passwd, useradd, userdel, su, doas, sudo), `keymaps`, `tzdata` |
| standard | the minimal group, `net` (dhcpc, ping, nc, http, net, xfer), `textutils` (sed, awk, tar, less, man), `locales`, `edit`, `games` |
| desktop | the standard group, `libwire`, `libfont`, `libcodec` with the PNG, SVG and BMP modules, `codecs-audio`, `libaudio`, `audiod`, `libgui`, `x12`, `desktop` (panel, desktop, greeter, startgui, settings, x12settings), `term`, `files`, `clock`, `imed` with its dictionaries, `fonts`, `theme` (icons and wallpapers), `sounds`, `diagnostics` (sysmon, logview, evtest, wireview, screenshot) |
| devel | `tcc`, `make`, `binutils` (ar, as, ld), `libc-dev` and one `-dev` package per library with its headers and static archive |
| none | `lua`, `lua-gui`, `lua-audio`, `profiler` (libprof, prof and the profiler window), the fourteen applications, and the metapackage `apps` that depends on all of them |
| none | `tests`, every test program, fixture and script, built only with `CONFIG_TESTS=1` and never placed in a release repository |

- An installed disk is partitioned with GPT. On x86_64 it begins with a
  BIOS boot partition of 1 MiB. Every installed disk has an EFI system
  partition of 256 MiB formatted as FAT32 and mounted at `/boot`, a swap
  partition and an mfs root partition, which also holds `/home`.
- The kernel takes root from `root=PARTUUID=UUID`. Without that option
  it looks for the root partition type of the Discoverable Partitions
  Specification for its architecture on the disk it was booted from, and
  failing that it mounts the whole `vda` as now, which is why test images
  need no partition table. Swap uses the swap partition type of the boot
  disk, or else the whole `vdb` as now.
- The installer medium `installer.img` is a GPT disk image, the form
  that is written to a USB stick. Its EFI system partition holds Limine,
  the kernel and the initrd of the installer environment, and a second
  partition holds the signed repository as an mfs filesystem. QEMU
  attaches it as a virtio disk. The installer verifies the repository
  index against the keys in its own initrd.
- The installer has a back end and a text front end. An answer file
  drives it without questions, which is how the boot tests use it. A
  graphical front end on the same back end is the last milestone.
- Development runs boot `build/dev.img`, one per architecture, which the
  unattended installer creates once. `make run` builds the repository and
  attaches it as an update medium, and a service at boot applies the
  updates before the login, the offline update model of Fedora. It
  reboots once when `kernel`, `limine` or `libc` changed. Boot tests boot
  images assembled on the host from the packages and do not use the
  development disk.

## 3. Milestones

### P0. Package format 2 and a host build of pkg

- `pkg` compiles for the host as `build/host/pkg` from `user/pkg/*.c`,
  `libc/src/gzip.c`, `libc/src/crypto/*.c` and `libc/src/net/http.c`.
- `--root DIR` becomes the installation root for the files, the records,
  the configuration and the keys, and replaces `--prefix`.
- Format 2, the records in `/var/lib/pkg`, owners and modes, the
  transactional installation, configuration files and the new library
  rule.
- `--perms-out FILE` writes the owners and modes of the installed files
  in the format of `user/perms`, for `mkfs -p`.
- `tools/mkpkg.sh` writes archives of format 2 with owner 0 and the modes
  of the build tree.
- The boot test `pkg` gains configuration files, a failed upgrade that
  leaves the old files in place, and owners and setuid bits.
  `make check-pkg` runs the same script against the host build.
- Until P3 the root image is still a copy of the build tree, and packages
  installed in a development run disappear with the next build.

### P1. Merged /usr

- The install paths of `user/Makefile` follow the layout of section 2,
  and the root tree carries the `/bin` and `/lib` links.
- `lib_dirs` in `user/ld/ld.c`, `CODEC_DIR` in
  `libcodec/include/codec/codec.h`, the paths of `TCC_DEFS` in
  `user/Makefile`, `PATH` in `/etc/profile`, the search path of `man` and
  the font paths of libfont and libgui follow the new layout.
- The boot tests `boot`, `dynlink`, `shell`, `tcc`, the `codec` cases,
  `gui`, `gui_term` and `login_console` must pass.

### P2. Splitting libc and Lua

- `libc/src/profile.c`, `profanalyze.c` and `profreport.c` move to
  `libprof/`, built as `libprof.so` with the header `prof/profile.h`, and
  `prof` and the profiler window link it.
- The gui, paint, image and audio bindings of Lua become modules in
  `/usr/lib/lua/5.5/`, loaded through `LUA_USE_DLOPEN` and a C module
  path. The MIME functions move from `user/lua/lsys.c` into the gui
  module. `lua` then links liblua and libc only.
- The boot tests `proftest`, `profreporttest`, the `lua` cases and
  `luasynth` must pass.

### P3. The base system as packages and the image builder

- Every package and group of section 2 has a manifest under
  `user/packages/`.
- `tools/mkimage.sh SET OUT` installs a set of packages from
  `build/repo` into `build/sysroot` with `build/host/pkg --root` and
  writes the image with `mkfs -p` and the generated owners. The rescue
  initrd is built from the minimal group in the same way.
- `build/initrd_root` and its install rules are removed. `make image` and
  `make test` build `build/disk.img` from the desktop group and `tests`.
- The boot tests `pkg`, `pkg_repo` and `pkg_apps` check that their
  packages are absent instead of checking for an empty database.
- The 32 release cases must pass on both architectures, together with
  `pkg` and `pkg_apps`.

### P4. Partitions and discovery in the kernel

- The block layer reads GPT, with the protective MBR, the CRCs of the
  header and the entries, and the backup header as fallback, and
  registers the partitions as `vda1`, `vda2` and onward. `/dev/partitions`
  lists the name, partition UUID, type UUID and size of each.
- A Limine executable file request identifies the boot disk, and root
  and swap are chosen as section 2 describes.
- A tmpfs is mounted at `/tmp` and `/run`.
- `fsinit` accepts `PARTUUID=UUID` as the device of an fstab entry.
- The host tool `tools/mkgpt` writes GPT disk images for the tests.
- The locks of the partition list and of tmpfs are recorded in
  `docs/design/locking.md` before they are added.
- The boot test `gpt_boot` boots a disk written by `tools/mkgpt`, with
  root on its root partition, swap on its swap partition, `/boot` mounted
  from its EFI system partition and a writable `/tmp`.

### P5. Disk tools on minios

- `part` creates, lists and edits GPT partition tables, aligned to 1 MiB.
- `mkfs` and `mkfat` are built for the target from the sources of
  `tools/mkfs` and `tools/mkfat` as well as for the host.
- The `limine` utility is built for the target from
  `third_party/limine/limine.c` for `bios-install`.
- The boot test `disk_tools` partitions and formats a blank `vdb` in the
  guest, and its `post` script checks the result with the host `mkfs
  --cat` and `fsck`.

### P6. Boot packages

- The package `kernel` installs `/boot/minios/kernel.elf`, and the
  package `limine` installs the EFI binaries and `limine-bios.sys`.
- A trigger of `pkg` rewrites `/boot/limine.conf` with an entry for the
  current kernel and a fallback entry for the previous one, with the
  command line from `/etc/kernel/cmdline`. On x86_64 it also runs
  `limine bios-install` again.
- `tests/run_qemu_test.sh` gains a second boot from the case's disk
  without the test ISO, selected by the case file `boot2`.
- The boot test `kernel_upgrade` installs a newer kernel package, boots
  it in the second boot and checks the fallback entry.

### P7. The installer

- `user/installer/` holds the back end and the text front end. The
  installer asks for the language, the keymap, the time zone, the target
  disk, an optional existing data volume to be reused as `/home` with its
  accounts imported from `/home/.local/etc`, the host name, the root
  password, the first user, who joins the group `wheel`, and the package
  groups.
- After a summary and a confirmation it partitions and formats the disk,
  mounts it, runs `pkg --root /mnt/target install`, writes `/etc/fstab`,
  creates the accounts and installs the boot loader. It writes its log to
  `/var/log/installer.log` on the target.
- The kernel option `installer=auto` makes it read the answer file from
  the repository partition.
- `make installer` builds `build/installer.img`.
- The boot test `install_auto` installs the desktop group onto a blank
  disk and must reach the greeter in the second boot. `install_console`
  installs the standard group and must reach `minios login:`.

### P8. The development disk and offline updates

- `make devdisk` creates `build/dev.img` with the unattended installer.
- `tools/run.sh` and `make run` attach the build repository as an update
  medium and boot the development disk.
- The service `pkg-update` applies the updates at boot before the login
  and reboots once when `kernel`, `limine` or `libc` changed.
- `make run KERNEL=build` boots the kernel of the build directly with
  the development disk as root.
- An existing `data.img` moves to the new layout through the reuse
  option of the installer.
- The boot test `offline_update` boots an installed image with a
  repository disk that holds a newer package and checks that the package
  is upgraded before the login.

### P9. The release and the freshness of the index

- `tools/release.sh` builds the repository and `installer.img`, and its
  boot check becomes `install_auto`.
- The release consists of `installer.img.gz`, the repository,
  `kernel.elf`, the public key, `BUILDINFO` and `SHA256SUMS`. The ISO and
  `root.img.gz` are no longer produced.
- The index carries a sequence number and an expiry date, `pkg update`
  refuses an index older than the one it holds or past its expiry, and
  each key is bound to the repositories it signs.
- `packages.md`, `storage.md`, `build.md`, `users.md` and `dynlink.md`
  describe the result, `installer.md` describes the installer, and
  `CLAUDE.md` and `docs/plan/README.md` follow the new design.
- The boot test `pkg_repo` refuses an older index and an expired one.

### P10. The graphical installer

- A libgui front end on the back end of P7 runs in an X12 session of the
  installer environment, which then contains the desktop libraries and
  the fonts.
- The boot test `gui_installer` drives it through the installation of
  the desktop group.
