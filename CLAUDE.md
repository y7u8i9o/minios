# minios

A monolithic x86_64 kernel written in C, booted by Limine, running under QEMU, with an aarch64 port in progress (`docs/plan/arm64.md`). The design summary and the milestone record are in `docs/plan/` (index in `docs/plan/README.md`). Read the plan that contains a milestone before starting it and mark the milestone completed in that file.

## Design decisions (fixed)

- x86_64, and aarch64 from milestone A4 of `docs/plan/arm64.md` on (`make ARCH=aarch64`). QEMU only, Limine boot protocol, higher half kernel at 0xffffffff80000000. Generic code reaches the architecture only through the interface of `docs/design/arch.md`.
- C17 freestanding, compiled with x86_64-elf-gcc or aarch64-elf-gcc. Assembly in GNU as syntax (`.S` files).
- Processes own address spaces, threads are the scheduling unit. MLFQ scheduler, kernel is not preemptible, rescheduling happens on return to user mode.
- Buddy physical allocator, slab kernel heap, copy on write fork, swap to a virtio-blk swap device.
- VFS with mount points and devfs. Custom inode filesystem `mfs`. virtio-blk storage.
- POSIX subset syscalls, ELF64 user programs dynamically linked against the shared libraries in `/lib` (`init` and `/lib/ld.so` are static), own libc in `libc/`.
- Single user, no permission enforcement. IPv4 networking over virtio-net (`docs/design/network.md`), no IPv6, forwarding or TLS.
- SMP since milestone M18 (application processors started through the Limine MP protocol). All per CPU state is in `struct cpu`, reached through `cpu_current()` (the GS base on x86_64, `TPIDR_EL1` on aarch64).

## Build and run

- `make` builds kernel, libc and user programs.
- `make image` builds the bootable image in `build/`.
- `make run` boots QEMU with serial on stdio.
- `make gdb` boots QEMU halted with the gdbstub on port 1234.
- `make test CASES="case ..."` runs the named boot tests from `tests/cases/`; `make test` alone runs all of them, which takes far too long and is not used.
- `VERSION` holds the semantic version (`MAJOR.MINOR.PATCH`), changed only when a release is cut; `BUILDNUM` counts kernel links on this machine and is not in git. See `docs/design/build.md`.

## Conventions

- Naming: `subsystem_verb_object` for functions, `struct name` without typedefs, fixed width integer types.
- Errors are negative errno values. Never return -1 without an errno.
- Every shared structure has a comment stating which lock protects it and acquires that lock from the first commit in which it exists, even while only one CPU runs. Interrupt disabling alone is never treated as mutual exclusion.
- Per CPU state is accessed only through `cpu_current()`. Never introduce a global `current` variable.
- Every unmap goes through `tlb_flush_range`, which becomes the IPI shootdown point at M17.
- Record lock ordering in `docs/design/locking.md` before adding a lock.
- No floating point in the kernel. No dynamic allocation before the slab allocator is initialized.
- Each milestone adds a boot test under `tests/cases/` that prints `TEST PASS` or `TEST FAIL <reason>` on serial and exits through isa-debug-exit.
- Each milestone adds a document in `docs/design/` describing the subsystem as implemented.
- Reference xv6 for structure and naming when in doubt. Do not copy code from xv6 or Linux.

## Working style

- Complete one milestone at a time in the order of its plan in `docs/plan/`. Do not start a later milestone before the boot test of the current one passes.
- Never run the full test suite. Before considering a change finished, run only the cases of the modules the change touches (`make test CASES="..."`, for example the `gui_*` and `comp_*` cases for a compositor change); pick them from the files changed, not by guessing.
- Do not impose a source-file line limit. Organize code around coherent responsibilities; preserve explanatory comments and avoid arbitrary splitting or tangled control flow.
