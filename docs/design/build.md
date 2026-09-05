# Build system and boot (M0)

## Toolchain

`toolchain.mk` is included by every Makefile. On x86_64 Linux it uses the
native GCC and GNU binutils, which can emit the freestanding x86_64 ELF files
directly. Other hosts default to the `x86_64-elf-` cross tools. Set `CROSS`
explicitly to override either choice (for example, `CROSS=x86_64-elf-`). It
also selects the host compiler for the helper programs under `tools/`, and
QEMU. Host utilities request POSIX.1-2008 declarations so they compile under
the strict C modes used by both glibc and macOS libc. The kernel is compiled
with:

    -std=c17 -ffreestanding -fno-stack-protector -fno-pic -fno-pie
    -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387
    -O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls
    -fno-asynchronous-unwind-tables -fno-strict-aliasing -fno-builtin

`-fno-omit-frame-pointer` and `-fno-optimize-sibling-calls` together keep the
frame pointer chain intact for the backtrace code. `-mno-red-zone` is required
because interrupt handlers run on the interrupted stack.

Build time options are make variables with defaults in `toolchain.mk` and are
passed to the compiler as `CONFIG_*` macros:

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
| `make run` | builds the image and boots it through `tools/run.sh` (see below) |
| `make gdb` | boots with `-s -S` and prints the GDB command line |
| `make test CASES="case ..."` | runs the named cases under `tests/cases/` (without `CASES` every case, which is too slow to use) |
| `make clean` | removes `build/` |

`make image` accepts `CMDLINE="..."` to set the kernel command line and
`VIDEO=WxH[xBPP][@SCALE]` to select the framebuffer mode (`video=` on the
command line, applied by Limine). `@2` asks the console and the compositor
to draw every pixel twice, see "High density displays" below.

## Running

`tools/run.sh` builds the QEMU command line for `make run` and `make gdb`:
`-M q35`, the accelerator, memory, CPU count, `-serial stdio`, the root and
swap virtio-blk devices, a virtio-snd device with an audio backend and the
ISO. Settings come from four layers, each overriding the previous one:

1. built-in defaults: 512M, 4 CPUs, HVF on macOS when the QEMU binary offers
   it and TCG elsewhere, Core Audio on macOS and on Linux the first of
   pipewire, pulseaudio (`pa`), alsa and sdl that the QEMU binary offers,
   `none` when it offers none of them (`QEMU_ACCEL=kvm` opts into KVM on
   Linux);
2. `qemu.conf` in the repository root, a shell fragment that is ignored by
   git (`qemu.conf.example` lists every setting);
3. `QEMU_*` environment variables, also accepted on the make command line,
   for example `make QEMU_AUDIO=none run`;
4. command line options, passed through make as `RUNFLAGS`, for example
   `make RUNFLAGS="--audio wav --smp 2" run`.

| Option | Variable | Meaning |
|---|---|---|
| `--audio BACKEND` | `QEMU_AUDIO` | `-audiodev` backend for virtio-snd: `coreaudio`, `none`, `wav`, `pa`, `pipewire`, ... |
| `--audio-opts OPTS` | `QEMU_AUDIO_OPTS` | extra `-audiodev` properties such as `out.frequency=48000` |
| `--wav FILE` | `QEMU_WAV` | output of the `wav` backend, default `build/audio.wav` |
| `--no-sound` | `QEMU_SOUND=0` | boot without a virtio-snd device |
| `--mem SIZE`, `--smp N`, `--accel NAME` | `QEMU_MEM`, `QEMU_SMP`, `QEMU_ACCEL` | machine |
| `--display SPEC`, `--serial SPEC` | `QEMU_DISPLAY`, `QEMU_SERIAL` | `-display` and `-serial` arguments |
| `--full-screen` | `QEMU_FULLSCREEN=1` | `full-screen=on,zoom-to-fit=on` on the display |
| `--vga TYPE` | `QEMU_VGA` | `virtio` (default, run time modes through virtio-gpu), `std` or `none` |
| `--no-tablet` | `QEMU_TABLET=0` | no virtio tablet: the window grabs the mouse |
| `--video MODE` | `QEMU_VIDEO` | framebuffer mode `WxH[xBPP][@SCALE]`, applied when the image is built |
| `--extra ARGS` | `QEMU_EXTRA` | appended to the command line; the same as arguments after `--` |
| `--gdb` | | `-s -S`, what `make gdb` passes |
| `--build` | | run `make image VIDEO=$QEMU_VIDEO` first; what `make run` passes |
| `--dry-run`, `--verbose` | | print the QEMU command line |
| `--config FILE` | `QEMU_CONF` | read another configuration file |

The script rejects an audio backend that the QEMU binary does not list in
`-audiodev help`. `tools/run.sh --help` prints the full option list.

## Kernel link

`kernel/linker.ld` places the kernel at `0xffffffff80000000` with four
`PT_LOAD` segments: `.text` (RX), `.rodata` with `.limine_requests` (R),
`.data` with `.bss` (RW) and `.ksyms` (R). All sections are 4 KiB aligned.
`.ksyms` is last so that the second link pass, which fills it with the symbol
table, does not change the address of any other symbol. The link happens
twice:

1. Link with an empty `.ksyms` into `build/kernel/kernel_pass1.elf`.
2. `nm -n -S` on that file is piped through `build/host/gensyms`, producing
   `build/kernel/ksyms.bin`, which a generated assembly stub includes with
   `.incbin`. The final link produces `build/kernel.elf`.

## Limine

Limine binaries live in `third_party/limine/` (release v10.8.5, binary branch)
together with the protocol header `limine.h` from the `limine-protocol`
repository. The kernel requests base revision 3. `tools/mkiso.sh` builds the
ISO: it copies the kernel to `/boot/kernel.elf`, writes `limine.conf` with the
requested command line to `/boot/limine/`, adds the BIOS and UEFI boot images,
runs `xorriso`, and finishes with `limine bios-install`. The `limine` host tool
is compiled from `third_party/limine/limine.c` into `build/host/limine`.

At entry `start.S` switches to a 16 KiB static boot stack, clears `rbp` so
backtraces terminate, and calls `kmain`.

## High density displays

QEMU's cocoa window shows one guest pixel per screen pixel, so on a Retina
display the default 1024x768 mode covers a quarter of the screen. The
answer is a doubled mode: `make VIDEO=2560x1600@2 run` boots a 2560x1600
framebuffer whose `@2` suffix reaches the kernel as `bootinfo.fb_scale`.
The framebuffer console draws its glyphs at twice the size, `/dev/fb0`
reports `scale` in `struct fb_info`, and the compositor composes a
1280x800 desktop and writes every logical pixel as a 2x2 block. Windows
keep their size on screen and stay sharp; clients are unchanged. On macOS
`tools/run.sh` picks `2560x1600@2` by itself when the main display is a
Retina display and the cocoa window is used; `QEMU_VIDEO=1024x768` in
`qemu.conf` restores the plain mode. With the default `virtio-vga` the
kernel's virtio-gpu driver sets the `video=` size itself, so any mode
whose frame fits 16 MiB works, and the mode can be changed later from
Settings > Display (`display.md`). With `--vga std` only the VGA BIOS
modes exist, and 2048x1536 is not among them, which is why the doubled
default is 2560x1600. The `video=` mode is baked into the ISO, so
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
default, `zoom-to-fit=off`), so the 90 percent limit keeps it inside the
screen next to panels and the title bar. The mode is `@2` when the screen
has 150 dpi or more, is reported 3000 pixels wide or more, or `GDK_SCALE`
is 2, and `@1` otherwise. XWayland reports a scaled size: a 2560x1440
panel with 150 percent scaling appears as 3840x2160 with no physical
size, which gives `2560x1440@2`; a 2560x1440 laptop panel at 189 dpi
gives `2304x1296@2`; a 1920x1080 monitor gives `1728x968@1`. `QEMU_DISPLAY`
values other than gtk and sdl, and systems without `xrandr` or
`xdpyinfo`, keep the image default. Limine cannot set these modes on
`virtio-vga`, so the guest boots at 1024x768 and the kernel's virtio-gpu
driver switches to the mode a few seconds later; a gtk window on a
native Wayland session does not follow that change, so on Wayland
sessions the script sets `GDK_BACKEND=x11` and the window runs through
XWayland, where it does. `QEMU_VIDEO` in
`qemu.conf` or `--video` overrides the choice.
`tests/cases/comp_scale` boots `video=2560x1600@2` and checks that the
compositor's surface appears at doubled coordinates as uniform 2x2 blocks.

## Tests

`tests/run_qemu_test.sh` boots one case with `-display none` (and `-vga std`
unless the case's `vga` file says `virtio`; a `tablet` file attaches a
virtio tablet), serial output to
a file and `-device isa-debug-exit,iobase=0xf4,iosize=0x4`. A case directory
contains `cmdline`, `expect` (one extended regular expression per line, all
must match the serial log), optionally `reject` and `timeout`. Any line
containing `TEST FAIL` fails the case. `tests/run_all.sh` runs every case and
prints a summary. Test images and logs are written to `build/tests/<case>/`.

Tests use HVF on macOS when the installed QEMU offers it and TCG elsewhere.
Set `ACCEL` to override that choice. `make test-kvm` runs the cases listed
in `KVM_CASES` with `ACCEL=kvm`; run it on the Linux machine after every
change to the kernel, because TCG follows Intel semantics and hides
processor and hypervisor differences (`platform.md`). `make run` follows the same default and
uses `QEMU_ACCEL` for an override (see Running above).
