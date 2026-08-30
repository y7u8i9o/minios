# Build system and boot (M0)

## Toolchain

`toolchain.mk` is included by every Makefile. It selects the `x86_64-elf-`
cross tools, the host compiler for the helper programs under `tools/`, and
QEMU. The kernel is compiled with:

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
| `make run` | boots the ISO with `-M q35 -m 512M -serial stdio` |
| `make gdb` | boots with `-s -S` and prints the GDB command line |
| `make test` | runs every case under `tests/cases/` |
| `make clean` | removes `build/` |

`make image` accepts `CMDLINE="..."` to set the kernel command line.

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

## Tests

`tests/run_qemu_test.sh` boots one case with `-display none`, serial output to
a file and `-device isa-debug-exit,iobase=0xf4,iosize=0x4`. A case directory
contains `cmdline`, `expect` (one extended regular expression per line, all
must match the serial log), optionally `reject` and `timeout`. Any line
containing `TEST FAIL` fails the case. `tests/run_all.sh` runs every case and
prints a summary. Test images and logs are written to `build/tests/<case>/`.

Tests run under TCG for determinism. `make run` uses HVF when the installed
QEMU offers it and TCG otherwise (`QEMU_ACCEL` in `toolchain.mk`).
