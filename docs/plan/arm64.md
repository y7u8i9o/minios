# The aarch64 port

This plan adds aarch64 as a second architecture beside x86_64. Milestone
identifiers use the prefix `A` so that they do not renumber the development
milestones. Each milestone ends with boot tests and a document in
`docs/design/`, and is marked completed here when its boot tests pass.

## 1. Motivation and scope

The development host is an arm64 Mac, on which QEMU runs the x86_64 guest
with TCG only (`docs/postmortems/2026-09-06-pipe-release.md`). An aarch64
guest runs under `-accel hvf` at close to native speed, which shortens every
boot test and makes the desktop usable without the TCG cost. A second
architecture with a weaker memory model also exercises the lock-free paths of
M43 to M46 under orderings that x86 total store order does not produce.

The port covers the kernel, libc, the dynamic loader, the user programs and
the boot tests. The x86_64 build remains the default, and its boot tests pass at
every milestone.

## 2. Fixed decisions

- The target is QEMU `virt` with UEFI firmware (edk2) and Limine on aarch64.
  The Limine boot protocol, the higher half kernel and the Limine MP protocol
  are kept.
- Paging uses the 4 KiB granule with four levels. TTBR1 maps the kernel and
  TTBR0 maps user space, with ASIDs.
- Devices are found through the device tree that Limine passes. PCI uses
  ECAM.
- `make ARCH=aarch64` selects the port. `ARCH=x86_64` is the default.
- Architecture code is in `kernel/arch/$(ARCH)/`, including the
  architecture headers in `kernel/arch/$(ARCH)/include/arch/`. Generic code
  includes them as `<arch/...>` and uses only the interface listed in
  `docs/design/arch.md`.
- The fixed design decision "x86_64 only" in `CLAUDE.md` is amended when A4
  is completed.

## 3. Milestones

Milestones A0 to A3 change only the x86_64 build. They move every
architecture dependency behind an interface, so that A4 to A9 add a second
implementation instead of editing generic code.

### A0. Architecture boundary for CPU and execution state (completed 2026-10-01)

- `toolchain.mk` defines the `ARCH` variable. Architecture headers move to
  `kernel/arch/x86_64/include/arch/`, the linker script to
  `kernel/arch/x86_64/linker.ld`. The machine name and the ELF machine number
  come from the architecture.
- Trap frame accessors (`<arch/frame.h>`) replace every access to an x86
  register field in generic code: system call number, arguments and return
  value, program counter, stack pointer, frame pointer and the user mode test.
- System call initialisation and the sysret restriction move into the
  architecture.
- Signal frame construction and restoration move into the architecture.
- Thread context initialisation, user frame initialisation, FPU state, the
  TLS base and the context switch steps become `arch_*` operations. The
  thread structure embeds `struct arch_thread`.
- Barriers, `cpu_relax`, interrupt state save and restore, and the cycle
  counter used by lock statistics become architecture primitives. Spinlocks
  use compiler atomics instead of inline `xchg`.
- Power off and reboot go through `platform_*` functions. The page fault
  error code is decoded by the architecture.
- The boot test is `arch`, and the design document is `docs/design/arch.md`.

Completed on `bleeding-edge-arm64`. The cases `arch`, `boot`, `cpu`,
`exception`, `backtrace`, `fork`, `libc`, `signals`, `fpu`, `pthreads`,
`dynlink`, `smp`, `smp_user`, `lockfree`, `sched`, `timer`, `time`, `vmm`,
`swap`, `hugepages`, `mmap_file`, `madvise`, `shutdown`, `shutdown_cmd`,
`blk`, `net_virtqueue`, `kbd`, `mouse`, `gpu_mode` and `profile` pass, as
does `make check`. `push_cli` and `pop_cli` are not renamed because the
profiler identifies lock primitives by symbol name (`docs/design/arch.md`
section 6). The `madvise` case had failed since the test programs became
dynamically linked: the page after its test region belonged to a shared
library mapping, so the "range past the mapping" check could not fail as
expected. The test now unmaps a guard page after the region.
`make test-kvm` (`docs/design/platform.md` rule 5) has not been run, because
it requires the Linux machine.

### A1. Platform boundary (completed 2026-10-01)

- The serial, debug-exit, CMOS RTC, PS/2 and PCI configuration drivers move
  into `kernel/arch/x86_64/` behind `platform_*` interfaces.
- Generic interrupt numbers with an allocator replace `VIRTIO_VECTOR_BASE`.
  `arch_send_ipi` replaces direct LAPIC calls, and MSI messages are composed
  by the architecture.
- The TSC and LAPIC timer become an architecture clocksource and tick timer.
- `struct cpu` is split into a generic per CPU structure and `struct
  arch_cpu`.
- The Limine boot information (memory map, framebuffer, initrd) is exposed
  through a generic structure.
- The boot test is `platform`.

The serial, debug exit, RTC, PS/2 and PCI configuration code is in
`kernel/arch/x86_64/` behind `<arch/platform.h>`. `irq_alloc` and
`arch_send_ipi` are in `<arch/irq.h>`, the clock and the tick in
`<arch/timer.h>`, `struct cpu` in `include/cpu.h` with `struct arch_cpu`,
and the start-up sequence in `init/main.c` with the hooks of
`<arch/init.h>`. The boot information needed no new structure: Limine is
the boot protocol of every architecture, so `struct bootinfo` moved
unchanged to `include/boot.h` and the Limine requests to
`init/bootinfo.c`. The cases `platform`, `arch`, `boot`, `cpu`,
`exception`, `backtrace`, `timer`, `time`, `kbd`, `mouse`, `mouse_wheel`,
`input`, `input_keyboard`, `input_tablet`, `blk`, `gpu_mode`, `audio_pcm`,
`net_virtqueue`, `net_nic`, `smp`, `smp_user`, `sched`, `lockfree`, `fork`,
`signals`, `libc`, `shutdown`, `shutdown_cmd`, `swap` and `profile` pass.

### A2. MMU boundary

- A portable page table entry interface (present, accessed, dirty, swap entry
  encoding) and a range walker replace the open-coded four level walks.
- `mm/vma.c`, `mmap.c`, `swap.c`, `huge.c`, `madvise.c`, `filemap.c` and
  `vmm.c` use only that interface.
- `memlayout.h` and the user and kernel address constants move into the
  architecture.

### A3. User ABI boundary

- The architecture code of libc moves to `libc/arch/x86_64/`: the system
  call stub, crt0, crti, setjmp, fenv, the x87 and SSE math assembly, the
  thread pointer read and the spin hint.
- The relocation types and `start.S` of the dynamic loader move to an
  architecture directory.
- `minios/simd.h` selects SSE2 or NEON per architecture.
- `ARCH` selects `UCFLAGS` and `QEMU`.

### A4. aarch64 toolchain and boot to serial

- The build uses `aarch64-elf-gcc` with `-mgeneral-regs-only` for the
  kernel, the Limine aarch64 image and edk2 firmware.
- The early kernel writes to the PL011, installs exception vectors, and
  prints a register dump and panics on an unexpected exception.
- The test harness selects the machine by `ARCH` and exits through PSCI
  `SYSTEM_OFF` or semihosting.
- The boot test `boot` passes on aarch64.

### A5. aarch64 memory, interrupts and time

- The MMU is enabled with ASIDs, and the physical and slab allocators run
  on it.
- The kernel implements a GIC driver for interrupts. The GIC version supported by the hvf
  backend of the installed QEMU is determined at the start of this
  milestone.
- The kernel uses the generic timer as the clocksource and for the tick.
- The boot tests `pmm`, `vmm`, `slab`, `timer` and `exception` pass on aarch64.

### A6. aarch64 threads and user mode

- The architecture implements the context switch, EL0 entry, `svc` system
  calls and signal frames.
- FP and SIMD state is enabled through CPACR_EL1, and the TLS base is kept
  in TPIDR_EL0.
- libc and the dynamic loader implement AArch64 relocations and TLS variant I.
- The boot tests `fork`, `libc`, `signals`, `pthreads` and `dynlink` pass on aarch64.

### A7. aarch64 devices

- The kernel parses the device tree, enumerates PCIe through ECAM, and
  drives virtio blk, net, input, gpu and snd and the PL031 RTC.
- The system boots to the shell and to the desktop.
- The boot tests `blk`, `time`, `gpu_mode`, `input_keyboard` and `gui` pass on aarch64.

### A8. aarch64 SMP

- The application processors are started through the Limine MP protocol.
- IPIs are sent as SGIs, and `tlb_flush_range` uses broadcast TLBI.
- The memory ordering of the lock-free paths of M43 to M46 and of the
  spinlock is audited.
- The boot tests `smp`, `smp_user`, `lockfree` and `sched` pass on aarch64.

### A9. aarch64 user tools

- tcc is built with its arm64 backend, and the `as` and `ld` wrappers and
  their manual pages are updated.
- Package repositories are kept per architecture.
