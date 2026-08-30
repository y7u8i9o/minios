# minios

A monolithic x86_64 kernel written in C, booted by Limine, running under QEMU. The full design and milestone list is in PLAN.md. Read PLAN.md before starting any milestone and mark completed milestones there.

## Design decisions (fixed)

- x86_64 only, QEMU only, Limine boot protocol, higher half kernel at 0xffffffff80000000.
- C17 freestanding, compiled with x86_64-elf-gcc. Assembly in GNU as syntax (`.S` files).
- Processes own address spaces, threads are the scheduling unit. MLFQ scheduler, kernel is not preemptible, rescheduling happens on return to user mode.
- Buddy physical allocator, slab kernel heap, copy on write fork, swap to a virtio-blk swap device.
- VFS with mount points and devfs. Custom inode filesystem `mfs`. virtio-blk storage.
- POSIX subset syscalls, static ELF64 user programs, own libc in `libc/`.
- Single user, no permission enforcement. No networking.
- SMP since milestone M18 (application processors started through the Limine MP protocol). All per CPU state lives in `struct cpu` accessed through the GS base.

## Build and run

- `make` builds kernel, libc and user programs.
- `make image` builds the bootable image in `build/`.
- `make run` boots QEMU with serial on stdio.
- `make gdb` boots QEMU halted with the gdbstub on port 1234.
- `make test` runs all boot tests in `tests/cases/`.

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

- Complete one milestone at a time in PLAN.md order. Do not start a later milestone before the boot test of the current one passes.
- Run `make test` before considering any change finished.
- Keep files under roughly 800 lines. Split by subsystem, not by arbitrary size.
