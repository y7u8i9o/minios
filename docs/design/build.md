# Build system and boot (M0)

## Toolchain

`toolchain.mk` is included by every Makefile. `ARCH` selects the target
(`x86_64` by default, `aarch64` for the port of `docs/plan/arm64.md`), and
an architecture other than x86_64 builds into `build/$(ARCH)/`. On a Linux host
of the target architecture it uses the native GCC and GNU binutils, which
can emit the freestanding ELF files directly. Other hosts default to the
`$(ARCH)-elf-` cross tools. `YACC` (default
`yacc`) generates the awk parser, and Berkeley yacc and bison both work. Set `CROSS`
explicitly to override either choice (for example, `CROSS=x86_64-elf-`). It
also selects the host compiler for the helper programs under `tools/`, and
QEMU. Host utilities request POSIX.1-2008 declarations, which lets them
compile under the strict C modes used by both glibc and macOS libc. The
kernel is compiled with the following flags.

    -std=c17 -ffreestanding -fno-stack-protector -fno-pic -fno-pie
    -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387
    -O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls
    -fno-asynchronous-unwind-tables -fno-strict-aliasing -fno-builtin

Kernel and user code is also compiled with `-ffile-prefix-map`, which
records paths below the source tree and the build directory relative to
them in debugging information and `__FILE__`. The binaries therefore
contain no path of the machine that built them.

`-fno-omit-frame-pointer` and `-fno-optimize-sibling-calls` together keep the
frame pointer chain intact for the backtrace code. `-mno-red-zone` is required
because interrupt handlers run on the interrupted stack.

User programs and shared libraries state their layout options instead of
relying on the linker's defaults, because the two toolchains differ. A
Linux distribution's ld enables RELRO and separate code segments and its
gcc links position independent executables, while the `x86_64-elf-` tools
do none of this. `ULAYOUT` passes `-z relro -z separate-code` to every
shared object and the loader, and programs also get `-no-pie`. Both hosts
thereby produce the same layout, and the boot tests on macOS exercise the
one a Linux host builds. Before this was stated, a Linux build produced
libraries whose RELRO range ended in the padding of their last page, which
`/lib/ld.so` refused (`dynlink.md`).

Build time options are make variables with defaults in `toolchain.mk` and are
passed to the compiler as `CONFIG_*` macros, as the following table lists.

| Option | Default | Effect |
|---|---|---|
| `CONFIG_TESTS` | 1 | compile `kernel/tests/` and run the test named by `test=` |
| `CONFIG_PANIC_EXIT` | 1 | panic exits QEMU through isa-debug-exit with code 1 |
| `CONFIG_LOCKDEBUG` | 1 | spinlock owner tracking and misuse checks |
| `CONFIG_LOG_LEVEL` | 1 | compile time klog threshold (0 debug to 3 error) |

## Targets

| Target | Result |
|---|---|
| `make` | `build/kernel.elf`, plus libc and user programs once they exist |
| `make image` | `build/minios.iso`, bootable under BIOS and UEFI |
| `make run` | builds the update medium and boots the development disk through `tools/run.sh` (see below) |
| `make run-image` | builds the image and boots the CD with the root image, as `make run` did before P8 of `docs/plan/packaging.md` |
| `make devdisk` | `build/dev.img`, the development disk, created once (see The development disk below) |
| `make clean-devdisk` | removes the development disk |
| `make updates` | `build/update.img`, the update medium with the packages of the build |
| `make gdb` | boots with `-s -S` and prints the GDB command line |
| `make test CASES="case ..."` | runs the named cases under `tests/cases/` (without `CASES` every case, which is too slow to use) |
| `make repo` | `build/repo/`, the signed package repository of the bundled applications (`packages.md`) |
| `make release` | the release of `VERSION` in `build/release/minios-VERSION` (see Releases below) |
| `make clean` | removes `build/` |

`make image` accepts `CMDLINE="..."` to set the kernel command line and
`VIDEO=WxH[xBPP][@SCALE]` to select the framebuffer mode (`video=` on the
command line, applied by Limine). `@2` asks the console and the compositor
to draw every pixel twice, see "High density displays" below.

## Running

`tools/run.sh` builds the QEMU command line for `make run` and `make gdb`,
which names the machine, the accelerator, memory, CPU count, `-serial
stdio`, the root and swap virtio-blk devices, a virtio-snd device with an
audio backend and the ISO. The machine follows `ARCH`. For x86_64 it is `-M q35` with
`-vga virtio`. For aarch64 (`make ARCH=aarch64 run`) it is `-M virt` with
GICv3 and without ACPI, `-cpu host` under HVF, the edk2 firmware that QEMU
installs (`EDK2_AARCH64` names another file) with `-boot
menu=on,splash-time=0`, which replaces the five second TianoCore screen of
its boot manager, virtio-gpu-pci as the
display with ramfb as the boot framebuffer, the ISO as a SCSI CD and
`-nic none` unless `--nic` names a network. Settings come from four
layers, each of which overrides the previous one.

1. The built-in defaults are 512M, 4 CPUs, HVF on macOS and KVM on Linux
   when the QEMU binary offers them and `/dev/kvm` is writable, TCG
   otherwise, the sdl display on Linux when the binary offers it, Core
   Audio on macOS and on Linux the first of pipewire, pulseaudio (`pa`),
   alsa and sdl that the binary offers, or `none` when it offers none of
   them.
2. `qemu.conf` in the repository root is a shell fragment that git
   ignores (`qemu.conf.example` lists every setting).
3. `QEMU_*` environment variables are also accepted on the make command
   line, for example `make QEMU_AUDIO=none run`.
4. Command line options are passed through make as `RUNFLAGS`, for
   example `make RUNFLAGS="--audio wav --smp 2" run`.

| Option | Variable | Meaning |
|---|---|---|
| `--audio BACKEND` | `QEMU_AUDIO` | `-audiodev` backend for virtio-snd, such as `coreaudio`, `none`, `wav`, `pa` or `pipewire` |
| `--audio-opts OPTS` | `QEMU_AUDIO_OPTS` | extra `-audiodev` properties such as `out.frequency=48000` |
| `--wav FILE` | `QEMU_WAV` | output of the `wav` backend, default `build/audio.wav` |
| `--no-sound` | `QEMU_SOUND=0` | boot without a virtio-snd device |
| `--mem SIZE`, `--smp N`, `--accel NAME` | `QEMU_MEM`, `QEMU_SMP`, `QEMU_ACCEL` | machine |
| `--display SPEC`, `--serial SPEC` | `QEMU_DISPLAY`, `QEMU_SERIAL` | `-display` and `-serial` arguments |
| `--full-screen` | `QEMU_FULLSCREEN=1` | `full-screen=on,zoom-to-fit=on` on the display |
| `--vga TYPE` | `QEMU_VGA` | `virtio` (default, run time modes through virtio-gpu), `std` or `none` |
| `--no-tablet` | `QEMU_TABLET=0` | no virtio tablet, and the window grabs the mouse |
| `--video MODE` | `QEMU_VIDEO` | framebuffer mode `WxH[xBPP][@SCALE]`, applied when the image is built |
| `--extra ARGS` | `QEMU_EXTRA` | appended to the command line, the same as arguments after `--` |
| `--gdb` | | `-s -S`, what `make gdb` passes |
| `--devdisk FILE` | `DEVDISK` | boot the development disk FILE instead of the CD, root image and swap |
| `--update FILE` | `UPDATE` | the update medium, attached as `vdb` beside the development disk |
| `--boot-kernel` | `RUN_BOOT=kernel` | boot the kernel of the CD with the development disk as the root |
| `--build` | | run `make image VIDEO=$QEMU_VIDEO` first, or `make devprep` with `--devdisk`, as `make run` does |
| `--dry-run`, `--verbose` | | print the QEMU command line |
| `--config FILE` | `QEMU_CONF` | read another configuration file |

The script rejects an audio backend that the QEMU binary does not list in
`-audiodev help`. `tools/run.sh --help` prints the full option list.

### The development disk

Since P8 of `docs/plan/packaging.md`, `make run` boots an installed
system that persists between runs and receives the packages of each
build through `pkg`, as an installed machine receives updates. `make
devdisk` creates the development disk once as `dev.img` in the build
directory of the architecture, a GPT disk of `DEVDISK_MB` (4096) MiB
that `tools/mkdisk.sh` writes from `build/sysroot` with a swap partition
of 256 MiB and the `video=` mode of `VIDEO`, and records the GUID of its
root partition in `dev.img.root`. `make clean-devdisk` removes it, and
the next run creates it again. `make updates` writes the update medium
`update.img` with `tools/mkupdate.sh`, a GPT disk with one partition of
the type repo, whose mfs contains the signed repository of the base
packages and the applications below `repo/ARCH`. `make run` runs `make
devprep`, which builds both, and boots the development disk from its own
boot loader with the medium as `vdb` and the data volume as `vdc`, where
`/etc/fstab` of the base system mounts it on `/home` as before.

At boot the task `pkg-update` installs the packages of the medium that
are newer than the installed ones (`packages.md`) and restarts the
machine when the kernel, the boot loader or libc changed. `-no-reboot` is
not passed for the development disk, and QEMU therefore starts the
machine again. The medium is attached writable, as a USB stick is,
because mounting an mfs updates its superblock, and make writes the
medium anew before every run. `make run BOOT=kernel` builds a CD with
the kernel of the build and the command line `root=PARTUUID` of the
development disk and boots it, which starts a new kernel without the
update and the restart. `make run-image` and `make gdb-image` boot the
CD with the root image of the tests, as `make run` did before.

## Kernel link

`kernel/arch/x86_64/linker.ld` places the kernel at `0xffffffff80000000`
with four `PT_LOAD` segments, `.text` (RX), `.rodata` with
`.limine_requests` (R), `.data` with `.bss` (RW) and `.ksyms` (R). All
sections are 4 KiB aligned. `.ksyms` comes last, which keeps the second
link pass, which fills it with the symbol table, from changing the
address of any other symbol. The link happens twice.

1. The first pass links with an empty `.ksyms` into
   `build/kernel/kernel_pass1.elf`.
2. `nm -n -S` on that file is piped through `build/host/gensyms`, producing
   `build/kernel/ksyms.bin`, which a generated assembly stub includes with
   `.incbin`. The final link produces `build/kernel.elf`.

## Version and build number

`VERSION` at the top of the tree holds the release under semantic
versioning (`MAJOR.MINOR.PATCH`) and is changed by hand when a release
is cut, on `develop` before the merge into `main`. `0.1.0` marked the
change from milestone numbers on 2026-09-05, `0.2.0` on 2026-09-30 the
dynamic loader, tcc, networking, packages, `dlopen` and init's service
supervision. `0.3.0` on 2026-10-03 added the aarch64 port, locales and
input methods, the codec library, the file chooser, multiple users with
su, doas and sudo, and the release pipeline. `0.3.1` on the same day
lifted the limit of the initrd, required a password at the first login,
added the GICv2, and left the tests and the build paths out of the
release. Application packages take their version from `VERSION`
unless `packages.mk` sets one. `tools/version.sh` runs before every
first pass link. It increments `BUILDNUM` (a counter local to the working
tree, ignored by git), reads the short commit hash and marks it
`-dirty` when tracked files differ from HEAD, and writes
`build/kernel/version.c` with `kernel_release`, `kernel_version`
(`#build commit date`) and `kernel_build_number`. `uname` reports the
release and version, the boot log prints `minios 0.3.1 build N (#N
commit date) booting, gcc V`, and the System page of Settings shows
the same line.

## Limine

Limine binaries are in `third_party/limine/` (release v10.8.5, binary branch)
together with the protocol header `limine.h` from the `limine-protocol`
repository. The kernel requests base revision 3. `tools/mkiso.sh` builds the
ISO. It copies the kernel to `/boot/kernel.elf`, writes `limine.conf` with the
requested command line to `/boot/limine/`, adds the BIOS and UEFI boot images,
runs `xorriso`, and finishes with `limine bios-install`. The `limine` host tool
is compiled from `third_party/limine/limine.c` into `build/host/limine`.

At entry `start.S` switches to a 16 KiB static boot stack, clears `rbp`,
which ends every backtrace, and calls `kmain`.

## High density displays

QEMU's cocoa window shows one guest pixel per screen pixel, and on a
Retina display the default 1024x768 mode therefore covers a quarter of the
screen. The answer is a doubled mode. `make VIDEO=2560x1600@2 run` boots a
2560x1600 framebuffer whose `@2` suffix reaches the kernel as `bootinfo.fb_scale`.
The framebuffer console draws its glyphs at twice the size, `/dev/fb0`
reports `scale` in `struct fb_info`, and the compositor composes a
1280x800 desktop and writes every logical pixel as a 2x2 block. Windows
keep their size on screen and stay sharp, and clients are unchanged. On
macOS `tools/run.sh` picks `2560x1600@2` by itself when the main display
is a Retina display and the cocoa window is used, and `QEMU_VIDEO=1024x768`
in `qemu.conf` restores the plain mode. With the default `virtio-vga` the
kernel's virtio-gpu driver sets the `video=` size itself, which makes any
mode whose frame fits 16 MiB work, and the mode can be changed later from
Settings > Display (`display.md`). With `--vga std` only the VGA BIOS
modes exist, and 2048x1536 is not among them, which is why the doubled
default is 2560x1600. Because the `video=` mode is baked into the ISO,
`--video` only takes effect when the image is built (`make run`, or
`tools/run.sh --build`). `--full-screen` is the alternative that scales
the plain mode to the screen with interpolation.

On Linux the gtk and sdl windows also show one guest pixel per screen
pixel, and a 2560x1440 laptop screen at 190 dpi shows the 1024x768 mode
as a small window. `tools/run.sh` therefore reads the primary screen from
`xrandr --current` (or `xdpyinfo`) and chooses the largest mode, with the
screen's aspect ratio, that is at most 90 percent of the screen in each
direction and whose frame fits the 16 MiB virtio-gpu buffer. The gtk
window resizes to the guest resolution on every mode change (QEMU's
default, `zoom-to-fit=off`), and the 90 percent limit keeps it inside the
screen next to panels and the title bar. The mode is `@2` when the screen
has 150 dpi or more, is reported 3000 pixels wide or more, or `GDK_SCALE`
is 2, and `@1` otherwise. XWayland reports a scaled size. A 2560x1440
panel with 150 percent scaling appears as 3840x2160 with no physical
size, which gives `2560x1440@2`, a 2560x1440 laptop panel at 189 dpi
gives `2304x1296@2`, and a 1920x1080 monitor gives `1728x968@1`. `QEMU_DISPLAY`
values other than gtk and sdl, and systems without `xrandr` or
`xdpyinfo`, keep the image default. Limine cannot set these modes on
`virtio-vga`, and the guest therefore boots at 1024x768 until the
kernel's virtio-gpu driver switches to the mode a few seconds later. A gtk
window on a native Wayland session does not follow that change, and an
sdl window there is sized in logical points, which makes a 2560x1440
guest fill a 2560x1440 screen with 150 percent scaling. The script
therefore prefers the sdl display on Linux and, on Wayland sessions, sets
`GDK_BACKEND=x11` and `SDL_VIDEODRIVER=x11`, which runs the window through
XWayland, sized in the pixels `xrandr` reported. A 2560x1440 guest then
covers two thirds of that screen.
`QEMU_VIDEO` selects a smaller mode when a smaller window is wanted. `QEMU_VIDEO` in
`qemu.conf` or `--video` overrides the choice.
`tests/cases/comp_scale` boots `video=2560x1600@2` and checks that the
compositor's surface appears at doubled coordinates as uniform 2x2 blocks.

## Tests

`tests/run_qemu_test.sh` boots one case with `-display none` (and `-vga std`
unless the case's `vga` file says `virtio`, while a `tablet` file attaches
a virtio tablet), serial output to
a file and `-device isa-debug-exit,iobase=0xf4,iosize=0x4`. A case directory
contains `cmdline`, `expect` (one extended regular expression per line, all
must match the serial log), optionally `reject`, `timeout` and `mem`, the
memory size in MiB, which `mem.ARCH` replaces on one architecture. The
cases `swap` and `madvise` boot with 128 MiB on x86_64 and 160 MiB on
aarch64, where the edk2 firmware and Limine hold the whole initrd in
memory before the kernel starts and run out of memory at 128 MiB. Any line
containing `TEST FAIL` fails the case. `tests/run_all.sh` runs every case and
prints a summary. Test images and logs are written to `build/tests/<case>/`.

Tests use HVF on macOS when the installed QEMU offers it and TCG elsewhere.
Set `ACCEL` to override that choice. `make test-kvm` runs the cases listed
in `KVM_CASES` with `ACCEL=kvm`. It belongs on the Linux machine after
every change to the kernel, because TCG follows Intel semantics and hides
processor and hypervisor differences (`platform.md`). `make run` follows the same default and
uses `QEMU_ACCEL` for an override (see Running above).

## Host checks

`make check` runs the font, protocol, GUI, Lua and signature checks on
the host, and `make check-pkg` alone runs the SHA-256, SHA-512 and Ed25519
vectors of RFC 6234 and RFC 8032 against `libc/src/crypto/`
(`packages.md`). `make check-sh` checks the shell parser, expansion and
execution. `make
check-net` self-tests the network peer harness with a fake QEMU, and `make
check-net-fuzz` fuzzes the network parsers under the sanitizers for three
seeds of `FUZZ_SECONDS` (30) each (`network.md`). On
Darwin, `toolchain.mk` adds `_DARWIN_C_SOURCE` to `HOSTCPPFLAGS` alongside
the POSIX feature level. This exposes native socket ancillary-data
macros, `RLIMIT_NPROC`, and `mkdtemp` without changing guest compiler flags.
The GUI and shell use private names for their host `strlcpy` helpers,
after including the system header and undefining any fortified macro.
GUI key fixtures use the input interface's `KEY_*` constants.

## Releases

`tools/release.sh`, also run as `make release` with its options in
`RELEASE_FLAGS`, builds the release that `VERSION` names from one commit,
`main` unless `--ref` names another. It checks the commit out with `git
worktree add --detach` into `build/release/work-VERSION`, which leaves the
checkout in use untouched and gives a kernel version without the `-dirty`
mark. The only file copied into the worktree is the tinycc submodule, and
the script refuses to start when the checked out submodule differs from
the one the commit records. Ignored files are not part of the release,
including the purchased sounds in `user/share/sounds`.

The pipeline then runs these steps and stops at the first failure, naming
the log of the step in `build/release/logs-VERSION`. The worktree is left
in place for inspection.

1. `make check` runs the host checks once.
2. For each architecture, x86_64 and aarch64 unless `--arch` limits them,
   `make test` runs the cases of `tests/release-cases` with the default
   build options. `--cases` replaces the list, and `--skip-tests` leaves
   out this step and the first.
3. `make image repo` builds the release into `build/release-ARCH` of the
   worktree with the options of `RELEASE_CONFIG`. By default the kernel
   self tests, the exit through isa-debug-exit, the lock debugging, the
   lock statistics and the slab debugging are off, and the log level is
   1. With `CONFIG_TESTS=0`, `user/Makefile` also leaves out the test
   programs of `user/tests`, `/etc/tests`, the loader and package test
   fixtures, the tcc test sources and the luasynth test scripts. The
   build directory is separate from the one of the boot cases,
   because the options are not dependencies of the objects.
4. `tools/run.sh` boots the release image without a data volume or
   sound and without a display window, and the step succeeds when the
   greeter reports its display server on the serial line, or the console
   login prints `minios login:`, within `BOOT_TIMEOUT` seconds (300). A kernel panic, an
   early exit of QEMU or the timeout fails it.

The kernels of a release report as their build number the number of
commits up to the released commit, which the script passes to
`tools/version.sh` as `BUILD_NUMBER`. Both architectures carry the same
number, and it grows from one release to the next.

The results go to `build/release/minios-VERSION`.

| File | Content |
|---|---|
| `minios-VERSION-ARCH.iso` | the bootable image with the kernel and the initrd |
| `minios-VERSION-ARCH-root.img.gz` | the root filesystem, compressed with gzip |
| `minios-VERSION-ARCH-kernel.elf` | the kernel with its debugging information |
| `minios-VERSION-ARCH-repo.tar.gz` | the signed package repository of the bundled applications |
| `minios-VERSION-signing.pub` | the public half of the signing key |
| `BUILDINFO` | the commit, the date, the build options, the compilers, the kernel version and the boot cases |
| `SHA256SUMS` | the SHA-256 sums of the other files |

The package repositories are signed with `RELEASE_KEY`, by default
`$HOME/.config/minios/release-signing.key`, which `pkgsign keygen`
creates on the first release. Every release must use the same key,
because the root image installs its public half as
`/etc/pkg/keys/build.pub` and installed systems verify the repository
index against it. `--tag` creates the annotated tag `vVERSION` at the
commit after a successful run, and an existing tag must already point at
that commit. The script pushes nothing. The worktree is removed after a
successful run unless `--keep` is given, and the script refuses to
overwrite an existing `minios-VERSION` or worktree.

A release follows the branch model below. `VERSION` is raised on
`develop`, `develop` is merged into `main` by hand, and `make release
RELEASE_FLAGS=--tag` builds and tags the release of `main`.

## Branches and the daily fold into develop

The repository has three long lived branches, `bleeding-edge` for new work,
`develop` for stabilised work and `main` for releases. Feature branches are
named `bleeding-edge-<feature>` or `develop-<feature>`.

`tools/fold-develop.sh` merges every `bleeding-edge` and `bleeding-edge-*`
branch into `develop` when at least one of them received a commit in the
last 24 hours. The merge runs in a temporary worktree, which leaves the
checkout in use untouched. A conflict aborts that merge, leaves `develop`
unchanged and exits with status 1. `--force` merges without a recent
commit, and `--push` (or `PUSH=1`) pushes `develop` to `origin` after a change. The script
appends one line per action to `~/Library/Logs/minios-fold-develop.log`.

The launchd agent `~/Library/LaunchAgents/com.minios.fold-develop.plist`
runs the script every day at 03:00 on this machine. `launchctl print
gui/$(id -u)/com.minios.fold-develop` shows its state, and
`launchctl kickstart gui/$(id -u)/com.minios.fold-develop` runs it at once.

## The data volume

`data.img` in the repository root is the persistent data volume
(`storage.md`), an empty mfs image that `make image` creates when it is
missing and never rebuilds. The aarch64 build uses `data-aarch64.img`,
because the volume contains the installed packages, which are built for
one machine. `DATA` names another file, `DATA_MB` its size
(256), and `make clean-data` removes it. `tools/run.sh` attaches it as the
third virtio-blk device, and `--data FILE` and `--no-data` override that.

## Header check

`make check-headers`, run by `make check`, compiles every header of
libc and of the libraries on its own with the cross compiler, as a
program compiled with tcc on minios sees them.

## Submodules

`third_party/tinycc` is a git submodule, which `git submodule update
--init` fetches after cloning. The other third party sources are copies fetched by the
scripts in `tools/`.
