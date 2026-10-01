# minios Development Plan

This directory contains the development plan of minios: the design summary, the
milestone record and the separate plans of larger features. Each milestone
ends with a visible result and a boot test in `tests/cases/`, and adds a
document in `docs/design/` that describes the subsystem as implemented. A
milestone is marked completed, with its date, in the file that contains it once
its boot tests pass.

## Contents

| File | Content | Status |
|---|---|---|
| `README.md` | Design summary, testing strategy, conventions and tooling requirements | maintained |
| `m00-m18-foundation.md` | M0 to M18: toolchain, console, memory, interrupts, scheduler, user mode, ELF, libc, VFS, virtio-blk, mfs, signals, windowing, SMP | completed 2026-08-29 |
| `m19-m26-display.md` | M19 to M26: windowing enhancements, fonts, application framework, widgets, the X12 display server rework | completed 2026-08-30 |
| `m27-m34-runtime.md` | M27 to M34: user floating point, libm, calculator, audio, virtio-gpu, HiDPI | completed 2026-09-03 |
| `m35-m47-kernel.md` | M35 to M47: POSIX threads, journaling, file mappings, madvise, huge pages, resource limits, profiler, lock statistics, lock-free paths, per CPU scheduler and allocators, input | completed 2026-09-05 |
| `features.md` | Work after M47, organised by feature on `bleeding-edge-*` branches | maintained |
| `network.md` | The TCP/IP plan, milestones N00 to N16 | completed 2026-09-30 |
| `network-progress.md` | Progress record and validation evidence of N00 to N16 | completed 2026-09-30 |
| `terminal.md` | The terminal userland plan | completed 2026-09-06 |
| `arm64.md` | The aarch64 port, milestones A0 to A9 | A0 to A4 completed 2026-10-01, A5 next |

Work proceeds one milestone at a time in the order of its plan. A later
milestone is not started before the boot tests of the current one pass.

## 1. Design Summary

| Area | Decision |
|---|---|
| Architecture | x86_64, QEMU only (`qemu-system-x86_64`, `-M q35`, `-accel hvf` where available). The aarch64 port (`arm64.md`) boots to serial on QEMU `virt` |
| Language | C (C17, freestanding), GNU as (`.S` files) for assembly |
| Boot | Limine bootloader, Limine boot protocol, higher half kernel at `0xffffffff80000000` |
| Kernel type | Monolithic |
| CPUs | SMP since M18, application processors started through the Limine MP protocol. All per CPU state kept in a `struct cpu` reached via the `GS` base |
| Preemption | Timer interrupts trigger rescheduling only on return to user mode. Kernel code is not preempted |
| Execution model | Processes with multiple threads. Threads are the scheduling unit, processes own the address space, file table and thread list |
| Scheduler | Multilevel feedback queue (MLFQ) |
| Process creation | `fork` and `execve`, copy on write fork |
| Physical memory | Buddy allocator |
| Kernel heap | Slab allocator on top of the buddy allocator |
| Virtual memory | Copy on write, swapping of anonymous pages to a swap partition |
| Filesystem | Custom inode based filesystem (`mfs`) behind a VFS with mount points and devfs |
| Storage | virtio-blk (PCI, modern virtio interface): the root image rebuilt by the build, a data volume mounted at `/home`, a swap device, FAT volumes |
| System calls | POSIX subset, entered through `syscall` / `sysretq` |
| Executables | ELF64, dynamically linked against the shared libraries in `/lib` since 2026-09-06 (`docs/design/dynlink.md`); `init` and the loader are static |
| libc | Own minimal libc (`libc/`) |
| User space | Shell with a line editor, coreutils, sed, awk, make, ar, tar, tcc, Lua 5.5, the package installer `pkg`, init with service supervision, desktop applications |
| Console | Framebuffer text console with bitmap font and 16 colour SGR, serial (COM1) mirror, timestamped kernel log |
| Input | Input core (`/dev/input/eventN`), PS/2 keyboard and mouse, virtio-input |
| Timer | TSC calibrated against the PIT for time, local APIC timer for the periodic tick |
| Networking | IPv4 over virtio-net and loopback: ARP, ICMP, UDP, TCP, DHCP, DNS (`network.md`). No IPv6, forwarding or TLS |
| Graphics | The X12 display server with a Wayland-like protocol (`protocol/`), the `libgui` toolkit, virtio-gpu mode setting with the std VGA framebuffer as fallback |
| Audio | virtio-snd through `/dev/pcm0`, the `audiod` mixing server, `libaudio` |
| Privilege | Ring 3 from the first user process |
| Permissions | Single user, permission bits stored but not enforced |
| Testing | Automated QEMU boot tests checked via serial output, exit via `isa-debug-exit` |
| Debugging | GDB attached to QEMU gdbstub, kernel panic with symbolized backtrace |
| Build | Plain Makefiles |
| Version control | git, no CI |
| Reference | xv6 (structure and naming), Linux documentation for buddy, slab and MLFQ details |

## 2. Repository Layout

The layout planned at M0 has grown past recognition. The current tree is
described under "Repository layout" in `docs/INTRODUCTION.md`, which is kept
with the code.

## 3. Testing Strategy

- `tests/run_qemu_test.sh <case>` boots the image with `-display none -serial file:<out> -device isa-debug-exit,iobase=0xf4,iosize=0x4` and a timeout. The kernel writes `TEST PASS` or `TEST FAIL <reason>` to serial and exits through port `0xf4`.
- Kernel self tests are compiled in when `CONFIG_TESTS=1` and selected by a Limine command line argument such as `test=pmm`.
- User space test programs in `user/tests/` are started by the `test=run prog=/bin/<name>` command line of a case and report the same markers.
- The network cases run against a peer on the host (`tools/netpeer`, `tests/net/`); `make check-net` checks the harness itself and `make check-net-fuzz` fuzzes the wire parsers on the host.
- `make check`, `make check-lua` and `make check-sh` run the host unit tests of the libraries, the Lua modules and the shell parser.
- `make test CASES="case ..."` runs the named cases and prints a summary. Only the cases of the modules a change touches are run; the full suite has grown too large to run for every change and is never run as a whole.
- The shutdown path is itself tested: a case boots to user space, runs `shutdown`, and asserts that the block cache was flushed (the mfs clean flag is set on the resulting image) and that QEMU exited through the ACPI power off rather than the timeout.

## 4. Conventions

- Kernel code is C17, freestanding, no dynamic allocation before M5, no floating point.
- Headers in `kernel/include/` use the `#pragma once` guard. Every subsystem has one header named after its directory.
- Naming: `subsystem_verb_object`, for example `pmm_alloc_page`, `vfs_open`, `sched_yield`. Types are `struct name`, no typedef for structs. Fixed width integers from `<stdint.h>`.
- Errors are negative `errno` values returned as `int` or `long`. Pointers returning errors use `ERR_PTR` and `IS_ERR` helpers.
- Locks: every shared structure documents which lock protects it in a comment above the struct definition, and takes that lock from the first commit in which it exists. Disabling interrupts is never treated as sufficient mutual exclusion on its own.
- Per CPU state is only accessed through `cpu_current()`. No global variables hold per CPU data.
- Lock ordering is recorded in `docs/design/locking.md` before a new lock is introduced.
- Every milestone adds a boot test and a short document in `docs/design/`.

## 5. Tooling Requirements

Required: `x86_64-elf-gcc`, `qemu-system-x86_64`, `xorriso` (`make image` builds the boot ISO), `yacc` (the awk grammar) and `python3` (`tools/xfer.py` and the network test scripts). `x86_64-elf-gdb` is needed for `make gdb`.
