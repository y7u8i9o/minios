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

### A2. MMU boundary (completed 2026-10-01)

- A portable page table entry interface (present, accessed, dirty, swap entry
  encoding) and a range walker replace the open-coded four level walks.
- `mm/vma.c`, `mmap.c`, `swap.c`, `huge.c`, `madvise.c`, `filemap.c` and
  `vmm.c` use only that interface.
- `memlayout.h` and the user and kernel address constants move into the
  architecture.
- The boot test is `pagetable`.

`<arch/paging.h>` defines `pte_t`, the table geometry and the entry
functions; the x86 bits are used only in `kernel/arch/x86_64/`.
`pt_next_leaf_table` (`mm/ptwalk.c`) replaces the open-coded walks of the
swap daemon and of `munmap`, and the recursive walks of `fork` and of the
teardown use the geometry. `struct vmspace` has `pt_root` in place of
`pml4_phys`. The region addresses are in `<arch/memlayout.h>`. The cases
`pagetable`, `vmm`, `munmap_tables`, `fork`, `swap`, `madvise`,
`hugepages`, `mmap_file`, `rlimit`, `signals`, `libc`, `libc_ext`,
`pthreads`, `dynlink`, `dlopen`, `smp`, `smp_user`, `sched`, `lockfree`,
`boot`, `shutdown`, `profile`, `fb0`, `fb_format`, `gpu_mode`, `mq`,
`pipes`, `mfs_user`, `fat_user`, `arch` and `platform` pass.

### A3. User ABI boundary (completed 2026-10-01)

- The architecture code of libc moves to `libc/arch/x86_64/`: the system
  call stub, crt0, crti, setjmp, fenv, the x87 and SSE math assembly, the
  thread pointer read and the spin hint.
- The relocation types and `start.S` of the dynamic loader move to an
  architecture directory.
- `minios/simd.h` selects SSE2 or NEON per architecture.
- `ARCH` selects `UCFLAGS` and `QEMU`.
- The boot test is `abi`.

libc takes `syscall.S`, the start files, `setjmp.S`, `fenv.c`,
`math_x87.c`, `math_long.c` and `libc_arch.h` from `libc/arch/x86_64/`; the
public headers take their architecture part from `include/bits/<arch>/`.
`minios/simd.h` has an SSE2 and a NEON implementation; the NEON one was
compiled with `aarch64-elf-gcc` but cannot run before A6. The loader takes
`start.S` and `ld_arch.h` from `user/ld/arch/x86_64/`. `toolchain.mk`
stops the build for an `ARCH` without flags. The cases `abi`, `libc`,
`libc_ext`, `float`, `fpu`, `mathvec`, `libmfull`, `pthreads`, `dynlink`,
`dlopen`, `signals`, `fork`, `tcc`, `lua`, `shell`, `utils`,
`shutdown_cmd`, `gui`, `gui_mandel`, `luasynth`, `pkg`, `boot`, `arch`,
`platform` and `pagetable` pass, as does `make check`.

### A4. aarch64 toolchain and boot to serial (completed 2026-10-01)

- The build uses `aarch64-elf-gcc` with `-mgeneral-regs-only` for the
  kernel, the Limine aarch64 image and edk2 firmware.
- The early kernel writes to the PL011, installs exception vectors, and
  prints a register dump and panics on an unexpected exception.
- The test harness selects the machine by `ARCH` and exits through PSCI
  `SYSTEM_OFF` or semihosting.
- The boot test `boot` passes on aarch64.

The whole generic kernel compiles and links for aarch64, with stubs that
name the milestone of every function not yet implemented. The PS/2 drivers
were split into the generic scancode and packet decoders in `drivers/` and
the 8042 code in `arch/x86_64/i8042.c`, because the self-tests feed input
through the decoders. The interface headers with identical prototypes moved
to `kernel/include/arch/`. `boot` and `exception` run as early self-tests
(`KTEST_DEFINE_STAGE` with `KTEST_EARLY`) after the boot environment is logged; on aarch64 they
pass under HVF on the Apple host, and `exception` shows the register dump,
the system registers and the backtrace (`tests/cases/exception/
expect.aarch64`). On x86_64 the cases `boot`, `exception`, `backtrace`,
`kbd`, `mouse`, `mouse_wheel`, `input`, `input_keyboard`, `input_tablet`,
`gui`, `gui_pointer`, `comp_seat`, `shutdown`, `shutdown_cmd`, `time`,
`timer`, `arch`, `platform`, `pagetable`, `abi`, `fork`, `signals`, `libc`,
`smp` and `lockfree` pass, as does `make check`.

### A5. aarch64 memory, interrupts and time (completed 2026-10-01)

- The MMU is enabled with ASIDs, and the physical and slab allocators run
  on it.
- The kernel implements a GIC driver for interrupts. The GIC version supported by the hvf
  backend of the installed QEMU is determined at the start of this
  milestone.
- The kernel uses the generic timer as the clocksource and for the tick.
- The boot tests `pmm`, `vmm`, `slab`, `timer` and `exception` pass on aarch64.

QEMU 11 offers GICv3 with an ITS under HVF, so the port uses GICv3. The
table walks moved from `arch/x86_64/paging.c` to the generic
`mm/pgtable.c`; the architecture keeps the entry format, the roots and the
TLB. The aarch64 processor of the Apple host manages neither the access
flag nor the dirty state, so a fault on an entry that permits the access
sets them (`update_access` in `mm/vma.c`). Self-tests run at stages of the
start-up sequence; `pmm`, `vmm`, `munmap_tables`, `slab`, `slab_redzone`
and `pagetable` run after the slab allocator and `timer` after the timer,
on both architectures, and `vmm` measures its second round, because the
first creates the kernel tables and slabs it uses. Under HVF the GIC of
Hypervisor.framework does not complete a write of `GICR_IGROUPR0`, so the
driver writes the redistributor registers only when their value differs.
The cases `boot`, `exception`, `pmm`, `vmm`, `munmap_tables`, `slab`,
`slab_redzone`, `pagetable` and `timer` pass on aarch64 under HVF and under
TCG. On x86_64 these cases and `kbd`, `fork`, `swap`, `madvise`,
`hugepages`, `mmap_file`, `rlimit`, `signals`, `libc`, `pthreads`,
`dynlink`, `smp`, `smp_user`, `lockfree`, `sched`, `shutdown`, `arch`,
`platform`, `abi`, `profile`, `gpu_mode` and `blk` pass.

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
