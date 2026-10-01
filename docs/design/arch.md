# Architecture interface

The kernel is divided into generic code and architecture code. The
architecture code of x86_64 is in `kernel/arch/x86_64/` and that of aarch64
in `kernel/arch/aarch64/`. The build selects the directory with the `ARCH`
variable of `toolchain.mk` (default `x86_64`): `kernel/Makefile` compiles
`arch/$(ARCH)` and adds `arch/$(ARCH)/include` to the include path after
`include`, so generic code includes `<arch/...>` without naming the
architecture. The linker script is `arch/$(ARCH)/linker.ld`.

The interface headers whose content is the same on every architecture,
prototypes only, are in `kernel/include/arch/`: `syscall.h`, `signal.h`,
`init.h`, `smp.h` and `platform.h`. Every other `<arch/...>` header is in
`kernel/arch/$(ARCH)/include/arch/`, and each architecture has one with the
same name. A header name exists in only one of the two directories.

Milestones A0 to A3 of the aarch64 port (`docs/plan/arm64.md`) introduced
the interface described here, and A4 added the aarch64 implementation
described in section 16. A0 covers the CPU and execution state: saved
registers, system call entry, signal frames, thread state, interrupt state,
barriers and page fault decoding. A1 covers the platform: the per CPU
structure, interrupt numbers and IPIs, the clock and the tick, the devices
of the PC and the start-up sequence. A2 covers the page tables and the
address layout. A3 covers the user ABI: libc, the dynamic loader and the
compiler flags. Generic code uses these operations and does not name an
x86 register, instruction, MSR, I/O port, APIC function or page table bit.
A second architecture implements the same headers with the same names.

Section 17 lists the dependencies that remain for the aarch64
implementation.

## 1. Machine identification (`<arch/machine.h>`)

`ARCH_MACHINE_NAME` is the machine string reported by `uname` (through
`KERNEL_MACHINE` in `kernel.h`). `ARCH_ELF_MACHINE` is the only `e_machine`
value that the ELF loader (`sched/elf.c`) accepts.

## 2. Saved register state (`<arch/frame.h>`)

A `struct trapframe` is saved at every trap, interrupt and system call. Its
layout belongs to the architecture. Generic code uses these accessors:

| Operation | Meaning | x86_64 |
|---|---|---|
| `frame_pc`, `frame_set_pc` | program counter | `rip` |
| `frame_sp`, `frame_set_sp` | stack pointer | `rsp` |
| `frame_fp` | frame pointer, the head of the unwind chain | `rbp` |
| `frame_from_user` | the frame was saved on an entry from user mode | `cs` RPL 3 |
| `frame_set_arg0` | first argument of the function entered with the frame | `rdi` |
| `frame_retval`, `frame_set_retval` | return value, 0 in the child of `fork` | `rax` |
| `frame_syscall_nr` | system call number | `rax` |
| `SYSARG0` to `SYSARG5` | system call arguments | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` |
| `arch_frame_init_user` | a frame that enters user mode at a given pc and sp, other registers zero, interrupts enabled | user selectors, `RFLAGS.IF` |
| `arch_fault_decode` | `struct fault_info` (present, write, user, exec) of a page fault | the `#PF` error code |

`include/syscall/syscalls.h` includes `<arch/frame.h>`, so every system call
handler reaches its arguments through `SYSARGn`. Because the number and the
result share `rax` on x86_64, `frame_syscall_nr` returns the result once a
system call has completed; generic code reads the number before the call.

## 3. System call entry (`<arch/syscall.h>`)

The entry stub (`arch/x86_64/syscall.S`) builds a trap frame and calls the
generic `syscall_dispatch` (`syscall/table.c`). Dispatch calls
`arch_syscall_enter(tf)` first, which records the entry for the panic dump.
It then enables interrupts, runs the handler from the table, stores the
result with `frame_set_retval`, calls `signal_deliver` and disables interrupts
again. A system call that replaced the whole register state, which is
`sigreturn`, finishes with `arch_syscall_return_full(tf)`. On x86_64 that
function enters user mode through `iretq`, because `sysret` cannot restore
`rcx` and `r11`. `syscall_init` and `syscall_init_cpu`, which program the
`syscall` MSRs, are architecture code (`arch/x86_64/sysentry.c`).

## 4. Signal frames (`<arch/signal.h>`)

Generic code (`ipc/signal.c`) chooses the signal, applies the disposition
and maintains the masks. `arch_signal_setup_frame` stores the interrupted
state, the FPU state, the saved mask and the signal number on the user
stack, and redirects the frame to the handler with the restorer as its
return address. It returns `-EFAULT` if the frame does not fit in writable
user memory. `arch_signal_restore_frame` reads the frame back for
`sigreturn`, sanitizes the privileged parts of the frame, reloads the FPU
state and returns the saved mask. The x86_64 layout is described in
`signals.md`. libc's `__sigreturn_trampoline` takes `SYS_sigreturn` from
`syscall_nums.h`, which therefore contains only `#define` lines so that
assembly can include it.

## 5. Thread state (`<arch/thread.h>`)

`struct thread` embeds `struct arch_thread` as `t->arch`. On x86_64 it
contains the 16 byte aligned `fxsave` area and the TLS base (the FS base). The
operations are:

| Operation | Use |
|---|---|
| `arch_thread_init(t, start)` | allocate the FPU state in its initial contents and prepare the kernel stack so that the first switch to `t` enters `start` (`thread_alloc`) |
| `arch_thread_free(t)` | release what `arch_thread_init` allocated (`thread_free`) |
| `arch_switch_to(prev, next)` | save `prev`'s FPU state and switch kernel stacks (`sched_switch_locked`) |
| `arch_thread_resume(t)` | load `t`'s FPU state and TLS base after every switch and in `thread_start` |
| `arch_set_kernel_stack(top)` | the stack used on entries from user mode (the TSS `rsp0`) |
| `arch_get_tls(t)`, `arch_set_tls(t, base)` | the TLS base; setting it for the calling thread also loads the register (`set_tls`, `fork`, `execve`, thread creation) |
| `arch_fpu_capture(t)` | store the calling CPU's FPU registers in `t`'s area (the child of `fork`) |
| `arch_fpu_reset(t)` | reset and load the initial FPU state of the calling thread (`execve`) |

## 6. Interrupt state, barriers and counters (`<arch/cpu.h>`, `<arch/barrier.h>`)

`arch_irq_enable`, `arch_irq_disable`, `arch_irqs_enabled`, and the pair
`arch_irq_save` and `arch_irq_restore` control the local interrupt state.
`arch_idle` enables interrupts and waits for the next one without losing a
wakeup in between (`sti; hlt`). `push_cli` and `pop_cli` (`include/cpu.h`,
`sched/cpu.c`) are the nesting interface used by spinlocks, implemented with
`arch_irq_save` and `arch_irq_flags_enabled`. The user space profiler
recognises lock primitives by these symbol names (`lockstat.md`), so the
names contain the x86 term "cli" on every architecture. Interrupt disabling is
never mutual exclusion on its own (`locking.md`).

`mb`, `rmb` and `wmb` order normal memory against device accesses, as in
the virtqueue code. On x86_64 they are `mfence`, `lfence` and `sfence`.
`cpu_relax` is the spin wait hint (`pause`). Data shared between CPUs is
ordered with the `__atomic` builtins. The spinlock uses
`__atomic_exchange_n` and `__atomic_store_n` with sequentially consistent
ordering, which compiles to the `xchg` the lock used before A0, so
acquisition and release remain full barriers. Whether acquire and release
ordering suffice is part of the memory ordering audit of A8.

`arch_cycles` is the uncalibrated cycle counter of the lock statistics
(`rdtsc`).

## 7. Per CPU state (`include/cpu.h`, `<arch/percpu.h>`)

`struct cpu` is generic. It contains the CPU id, the current and idle
threads, the kernel stack top, the interrupt nesting state, the loaded
address space, the scheduler, RCU and page cache fields, and the
architecture part `struct arch_cpu` as `c->arch`. On x86_64 that part
contains the scratch slot of the `syscall` entry, the local APIC id and the
diagnostics of the last entry from user mode. The fields up to `arch` have
fixed offsets because `syscall.S` reads `kstack_top` at offset 24 and
`arch.user_rsp` at offset 48; static assertions in `<arch/cpu.h>` check
both. The array of structures and `cpu_by_id` are in `sched/cpu.c`.
`cpu_current` is defined by the architecture (`%gs:0` on x86_64).

## 8. Interrupt numbers and IPIs (`<arch/irq.h>`)

An interrupt number is the value that `irq_register` takes and that a
handler is registered for; on x86_64 it is the IDT vector. Generic code
uses the named numbers `IRQ_TIMER`, `IRQ_RESCHED` and `IRQ_TLB_SHOOTDOWN`,
and allocates numbers for message signalled device interrupts with
`irq_alloc`, which virtio uses for MSI-X (vectors from 40 upward on x86_64,
LPIs from 8192 upward on aarch64).
`arch_send_ipi(cpu, irq)` sends an interrupt to a CPU by its kernel id; the
x86_64 implementation looks up the local APIC id in `c->arch`. The fixed
vectors of the PC devices and the APIC functions are in `<arch/apic.h>`,
which only architecture code includes.

## 9. Clock and tick (`<arch/timer.h>`)

`arch_clock_read` returns a free running counter and
`arch_clock_calibrate` returns its counts per millisecond. `drivers/timer.c`
converts the counter to milliseconds and nanoseconds since the kernel
entry. `arch_timer_init(hz, per_ms)` logs the clock and starts the periodic
tick of the boot CPU on `IRQ_TIMER`, and `arch_timer_init_cpu(hz)` starts
the tick of an application processor. On x86_64 the counter is the TSC,
calibrated against the PIT over 20 ms, and the tick is the local APIC timer
(`arch/x86_64/clock.c`). `arch_wait_for_interrupt` waits in the early boot
variant of `sleep_ms`.

## 10. Platform services (`<arch/platform.h>`)

| Function | Use | PC implementation |
|---|---|---|
| `platform_power_off`, `platform_reboot` | the `reboot` system call after the orderly shutdown (`signals.md`) | ACPI PM1a, 8042 reset (`console.md`) |
| `platform_test_exit(code)` | the exit of a boot test (`ktest.c`) and of a panic with `CONFIG_PANIC_EXIT` | isa-debug-exit on port `0xf4` |
| `platform_rtc_read` | `rtc_init` reads the date once (`time.md`) | CMOS clock (`cmos.c`) |
| `platform_pci_read32`, `platform_pci_write32` | configuration space accesses of `drivers/pci.c` | configuration mechanism 1, ports `0xcf8` and `0xcfc` (`pci_config.c`) |
| `platform_msi_compose(dev, irq, ...)` | the MSI-X table entries of `pci_msix_set_vector` | local APIC address `0xfee00000` with the APIC id, vector as data; `dev` is unused |
| `platform_devices_init` | devices that exist only on this platform | PS/2 keyboard and mouse |

The console UART implements `<drivers/serial.h>` (`arch/x86_64/serial.c`,
COM1). The PS/2 scancode and packet decoders are generic
(`drivers/ps2kbd.c`, `ps2mouse.c`) and the 8042 controller is PC code
(`arch/x86_64/i8042.c`). The kernel self-tests feed scancodes and packets
through the decoders; on a platform without an 8042, `ktest_run_selected`
registers the decoders as input devices before the selected test runs.

## 11. Start-up sequence (`<arch/init.h>`, `include/boot.h`)

The entry code of the architecture (`start.S`) calls the generic `kmain`
(`init/main.c`) on the boot stack. `kmain` calls the architecture at four
steps: `arch_init_cpu_boot` (descriptor tables and the boot CPU's
`struct cpu`), `arch_init_traps` (exception entry points),
`arch_init_cpu_features` (processor identification) and
`arch_init_interrupts` (interrupt controllers). It also calls
`smp_park_aps` and `smp_start_aps` (`<arch/smp.h>`) and
`platform_devices_init`. Every architecture boots through the Limine boot
protocol, so `struct bootinfo` and the Limine requests in
`init/bootinfo.c` are generic.

## 12. Page tables (`<arch/paging.h>`)

An entry is a `pte_t`. Generic code walks the tables through the geometry
`PT_LEVELS`, `PT_ENTRIES`, `PT_SHIFT(level)`, `PT_LEVEL_SIZE(level)` and
`PT_INDEX(va, level)`, where level 1 maps 4 KiB pages and a level 2 entry
is a table pointer or a 2 MiB block (`PAGE_2M`). The first
`PT_ROOT_USER_ENTRIES` entries of a root table map user space. The entry
format is private to the architecture. Generic code reads entries with
these functions:

| Function | True for |
|---|---|
| `pte_present` | an entry the hardware translates through |
| `pte_mapped` | an entry with a frame: present, or kept for a `PROT_NONE` region |
| `pte_is_table`, `pte_is_block` | a table pointer, a 2 MiB block (level 2) |
| `pte_write`, `pte_user`, `pte_young`, `pte_dirty` | the hardware permission and status bits |
| `pte_cow`, `pte_lazyfree`, `pte_protnone`, `pte_swapped` | the software states of `vma.c`, `madvise.c`, `filemap.md` and `swap.c` |

`pte_addr` returns the physical address, `pte_table` the next table
through the direct map, `pte_swap_slot` the slot of a swapped entry and
`pte_vm_flags` the `VM_*` flags of a present entry. Entries are built with
`pte_make(pa, vm_flags)`, `pte_make_protnone`, `pte_make_swap`,
`pte_make_table` and `pte_set_addr`, and changed with the `pte_mk*` and
`pte_clear_*` functions, `pte_wrprotect`, `pte_mkblock` and
`pte_block_to_page`. `vma_make_pte(pa, flags)` (`mm/vma.c`) gives the
entry of a user region: present, or `PROT_NONE` without `VM_READ`.

The table operations that only walk and allocate tables are generic
(`mm/pgtable.c`, A5): `paging_walk`, `paging_walk_preallocated`,
`paging_pde`, `paging_map_large`, `paging_alloc_table`, `paging_free_table`
and `paging_free_user_tables`. The architecture implements the roots and
the TLB: `paging_init_kernel_root`, `paging_init_user_root` (x86_64 copies
the kernel half of the root into every user root, aarch64 assigns an
ASID), `paging_release_user_root`, `paging_load`, `paging_flush_page`,
`paging_flush_user` and `paging_enable_features`.

A fault on an entry that permits the access sets the access flag, and for
a write the dirty state, before the region is consulted (`update_access` in
`mm/vma.c`). aarch64 needs this where the processor does not manage these
bits in hardware (FEAT_HAFDBS): a writable entry stays read only until its
first write, and an entry made old by the swap daemon faults on its next
access. On x86_64 the path is taken only after a stale TLB entry.
The x86_64 bit layout, including the software bits, is described in
`vmm.md`, `filemap.md`, `madvise.md` and `swap.md`.

`pt_next_leaf_table` (`mm/ptwalk.c`) returns the next level 1 table of a
range and skips absent upper tables and 2 MiB blocks. The swap daemon
(`find_victim`, `find_swapped`) and `munmap` (`detach_empty_pts`) use it.
The recursive walks of `fork` (`share_level`) and of the teardown of a
space (`free_user_level`) use the geometry and the entry functions.

## 13. Address layout (`<arch/memlayout.h>`)

The architecture defines the addresses of the regions: `HIGHER_HALF_BASE`,
`KERNEL_VBASE`, `USER_BASE`, `USER_TOP`, `USER_STACK_TOP`, `USER_MMAP_TOP`,
`USER_INTERP_BASE`, `KMMIO_BASE` and `KMMIO_SIZE`, `KSTACK_BASE` and
`KHEAP_BASE`. `mm/memlayout.h` and `mm/vmm.h` include it and define the
generic sizes (`USER_STACK_SIZE`, `KSTACK_SIZE`, `KSTACK_SLOTS`).

## 14. User ABI (`libc/arch/`, `user/ld/arch/`, `toolchain.mk`)

libc compiles the files of `libc/arch/$(ARCH)/` with the generic sources
and finds the internal header `libc_arch.h` through `-Iarch/$(ARCH)`:

| File | Content on x86_64 |
|---|---|
| `syscall.S` | `__syscall6` and the `sigreturn` trampoline |
| `crt0.S`, `crti.S`, `crtn.S` | the program entry and the static init sections |
| `setjmp.S` | `setjmp`, `longjmp`, `_setjmp`, `_longjmp` |
| `fenv.c` | the floating point environment over x87 and MXCSR |
| `math_x87.c` | `sqrt`, `sqrtf`, `__math_partial_remainder` (behind `fmod` and `remainder`), `atanl`, `atan2l` |
| `math_long.c` | the x87 long double functions |
| `libc_arch.h` | `__arch_thread_pointer`, `__arch_spin_hint`, `__arch_thread_stack_top` |

The public headers `setjmp.h` and `fenv.h` include
`bits/<arch>/setjmp.h` and `bits/<arch>/fenv.h`, selected by the compiler's
architecture macro. `minios/simd.h` includes the vector types of
`bits/simd_types.h` and the square root, minimum and maximum of
`bits/x86_64/simd.h` (SSE2) or `bits/aarch64/simd.h` (NEON). The NEON
minimum and maximum select lanes so that they return the same lanes as the
SSE instructions (`floating.md`).

The dynamic loader takes `start.S` and `ld_arch.h` from
`user/ld/arch/$(ARCH)/`. `ld_arch.h` maps the relocation types to the
generic names `RELOC_NONE`, `RELOC_ABS64`, `RELOC_COPY`, `RELOC_GLOB_DAT`,
`RELOC_JUMP_SLOT`, `RELOC_RELATIVE`, `RELOC_TLS_DTPMOD`, `RELOC_TLS_DTPREL`
and `RELOC_TLS_TPREL`, and defines `ld_arch_syscall`,
`ld_arch_thread_pointer` and `ld_arch_tls_tprel`.

`toolchain.mk` selects by `ARCH` the kernel flags `KARCHFLAGS`, the user
flags `UARCHFLAGS`, the tcc backend `TCC_TARGET` and the QEMU binary
`qemu-system-$(ARCH)`; `tests/run_qemu_test.sh` takes the QEMU binary from
`ARCH` too. An `ARCH` without these definitions stops the build.

## 15. Tests

The kernel self-test `arch` (`kernel/tests/test_arch.c`, case
`tests/cases/arch`) checks the A0 interface:
- the frame accessors on a frame built by `arch_frame_init_user`, the
  agreement between `frame_set_arg0` and `SYSARG0`, and the decoding of two
  page fault error codes;
- the nesting of `push_cli` and `pop_cli` and of `arch_irq_save` and
  `arch_irq_restore`;
- that `arch_set_tls` loads the register for the calling thread;
- that the `e_machine` field of `/bin/init` equals `ARCH_ELF_MACHINE`.

The kernel self-test `platform` (`kernel/tests/test_platform.c`, case
`tests/cases/platform`) checks the A1 interface:
- that `cpu_current` and `cpu_by_id` agree on every CPU;
- that the clock advances and a 20 ms sleep measures at least 20 ms;
- that `irq_alloc` returns two different numbers with an MSI address;
- that `platform_rtc_read` returns a plausible date;
- that the configuration space of the host bridge at 00:00.0 is readable.

The kernel self-test `pagetable` (`kernel/tests/test_pagetable.c`, case
`tests/cases/pagetable`) checks the A2 interface:
- that `pte_vm_flags` returns the flags given to `pte_make`, for user,
  kernel text, write combining and uncached entries;
- copy on write, accessed, dirty and lazy free changes and their reversal;
- the `PROT_NONE`, swap, block and table entries and `vma_make_pte` for an
  unreadable region;
- that `pt_next_leaf_table` returns exactly the three level 1 tables of
  pages placed under different level 4, level 3 and level 2 entries.

The user test `abitest` (`user/tests/abitest.c`, case `tests/cases/abi`)
checks the A3 interface: `setjmp` and `longjmp` across nested frames, the
directed rounding modes and the division by zero flag of `fenv.h`, the
lanes of the SIMD minimum and maximum with NaN and signed zeros, the array
square root tail, and the 16 byte stack alignment and the TLS
initialization of a new thread.

The behaviour of the moved code is covered by the existing cases of the
subsystems that use it: `fork`, `libc`, `signals`, `fpu`, `pthreads`,
`dynlink`, `smp`, `smp_user`, `lockfree`, `sched`, `exception`,
`backtrace`, `profile`, `vmm`, `swap`, `shutdown`, `blk`, `timer`, `time`,
`kbd`, `mouse`, `input`, `input_keyboard`, `input_tablet`, `gpu_mode`,
`audio_pcm`, `net_nic`, `net_virtqueue`, `munmap_tables`, `madvise`,
`hugepages`, `mmap_file`, `rlimit`, `fb0`, `float`, `fpu`, `mathvec`,
`libmfull`, `dlopen`, `tcc`, `lua` and `luasynth`.

## 16. The aarch64 implementation (A4 to A7)

The whole generic kernel, with its self-tests except `test_cpu.c`
(`TESTS_X86_ONLY` in `kernel/Makefile`), compiles and links for aarch64. The
kernel flags are `-march=armv8-a -mgeneral-regs-only -mno-outline-atomics
-mcmodel=small`. The functions of later milestones stop the kernel with
`ARCH_TODO` (`arch/aarch64/todo.h`), which names the function and the
milestone. Implemented are:
- the entry (`start.S`), which selects `SP_EL1` because Limine may enter
  with `SPSel` clear, and the boot stack with its guard page;
- the exception vectors (`vectors.S`), which save a `struct trapframe` and
  call `trap_dispatch`, and the register and system register dumps of
  `trap.c`;
- the boot CPU (`cpu.c`): `struct cpu` through `TPIDR_EL1`, `VBAR_EL1`, and
  the identification from `MIDR_EL1` and the ID registers;
- the console on the PL011 of `virt` (`serial.c`), mapped on first use by
  `early_map_device` (`early_mmio.c`), which adds device entries to the
  tables Limine installed in `TTBR1_EL1` and takes its tables from a static
  pool; the first load of the kernel root enters these mappings into it
  (`early_mmio_install`);
- power off, reboot and the test exit through PSCI (`platform.c`);
- the page tables (`paging.c`, A5): the entry format of the ARMv8
  descriptors, `MAIR_EL1` with write-back, Device-nGnRE and non-cacheable
  attributes, the kernel root in `TTBR1_EL1`, the user roots in `TTBR0_EL1`
  with 8 bit ASIDs (`asid_lock`, `locking.md`), an empty table in
  `TTBR0_EL1` while the kernel space is active, and the hardware access
  flag and dirty state where `ID_AA64MMFR1_EL1` reports them;
- the GICv3 (`gic.c`, A5): the distributor, the redistributor of the boot
  CPU and the CPU interface through the ICC system registers, with every
  interrupt in group 1. Under HVF the GIC of Hypervisor.framework does not
  complete a write of `GICR_IGROUPR0`, so the redistributor registers are
  written only when their value differs;
- the clock from `CNTVCT_EL0` and `CNTFRQ_EL0` and the tick from the
  virtual timer, PPI 27, programmed one period ahead through
  `CNTV_CVAL_EL0` (`clock.c`, A5);
- threads, user mode and signals (A6): the context switch of `context.S`
  (x19 to x30 and sp), the FP and SIMD state of `fpu.S` (q0 to q31, FPSR,
  FPCR, enabled through `CPACR_EL1.FPEN`), the TLS base in `TPIDR_EL0`,
  `user_enter`, which moves the frame to the top of the kernel stack so that
  `SP_EL1` is the stack top while the thread runs at EL0, system calls by
  `svc` with the number in x8, and the signal frame of `signal.c`, entered
  with the restorer in x30. The return path masks exceptions before it
  writes `ELR_EL1` and `SPSR_EL1`, and loads `SP_EL0` for every return to
  EL0t;
- the PL031 real time clock (`platform.c`, A6);
- the device tree (A7). Limine passes the flattened tree that edk2
  installs when the machine has no ACPI tables (`acpi=off`). The generic
  reader `kernel/lib/fdt.c` finds nodes by compatible string and decodes
  `reg` with the cell counts of the parent. `devtree_init`
  (`arch/aarch64/devtree.c`), called from `arch_init_cpu_features` before
  `pmm_reclaim_bootloader` frees the tree, records the addresses of the
  distributor, the redistributors, the ITS, the PL031 and the ECAM window
  with its bus range and `msi-map`; without a tree the addresses of `virt`
  apply and there is no PCI;
- PCIe configuration through ECAM (`platform.c`, A7): 1 MiB per bus,
  mapped at the first access of the bus under `ecam_lock`
  (`locking.md`), reads outside the bus range return all ones;
- MSI through the GICv3 ITS (`its.c`, A7). The redistributor of the boot
  CPU gets an LPI configuration table for 14 interrupt ID bits and a
  pending table. The ITS gets a command queue of one page, an indirect
  device table where the ITS supports one (otherwise a flat table for 16
  buses) and a collection table; collection 0 targets the boot CPU.
  `irq_alloc` hands out LPIs from 8192. `platform_msi_compose` translates
  the requester ID of the function through `msi-map` to a device ID, maps
  the device with an interrupt translation table of 32 events on first use
  (`MAPD`), maps the next event to the LPI (`MAPTI`, `INV`, `SYNC`) and
  returns the address of `GITS_TRANSLATER` with the event ID as data. The
  command queue and the tables are protected by `its_lock`; the data cache
  lines of every table entry the CPU writes are cleaned to the point of
  coherency, for an implementation that does not snoop them;
- `paging_sync_icache`, which cleans the data cache and invalidates the
  instruction caches for a frame the kernel wrote before it is mapped
  executable (the ELF loader, `vma_make_pte`); x86_64 needs nothing;
- one processor for `<arch/smp.h>` until A8.

Self-tests run at stages of the start-up sequence (`enum ktest_stage`,
`KTEST_DEFINE_STAGE`): `KTEST_EARLY` after the boot environment is logged
(`boot`, `exception`), `KTEST_MEMORY` after the slab allocator (`pmm`,
`vmm`, `munmap_tables`, `slab`, `slab_redzone`, `pagetable`), `KTEST_TIMER`
after the timer, with interrupts enabled for the test (`timer`), and
`KTEST_KINIT` in the first thread for every other test. A test runs at the
earliest stage at which what it uses is initialized. On aarch64 the kernel
reached the timer stage before A6, whose threads the scheduler needs;
since A6 it boots to the first thread and runs user programs, and since
A7 its root is the mfs on the virtio disk.

`make ARCH=aarch64 test CASES="..."` boots QEMU `virt` with the edk2
firmware that QEMU installs, on HVF where available (`-cpu host`, otherwise
TCG with `-cpu max`), without ACPI so that edk2 installs the device tree.
The image is the ISO of `tools/mkiso.sh`, whose UEFI El Torito image
contains Limine's `BOOTAA64.EFI`, attached as a SCSI CD. The machine has no
VGA: `ramfb` gives the boot framebuffer that std VGA gives on the PC, and a
case with `vga` set to `virtio` also gets `virtio-gpu-pci`, because edk2
offers no framebuffer on it. A case without a `nic` file gets `-nic none`,
since `virt` otherwise adds a virtio-net device. The ramfb framebuffer is
in RAM that the memory map reserves; `pmm_is_ram` is false for reserved
frames, so a mapping of it takes no page references, as for a framebuffer
in a PCI BAR.
Without user programs (`ARCH_USERLAND = no` in `toolchain.mk`) the initrd is
an empty archive and no disk is attached. The machine has one CPU until
the application processors are parked by the kernel (A8), because
`pmm_reclaim_bootloader` frees the memory in which Limine parks them; it
uses GICv3 (`gic-version=3`). A case may have an
`expect.$(ARCH)` file that replaces `expect` where the output names
architecture state, as `tests/cases/exception` and `tests/cases/timer` do.

## 17. Dependencies that remain

The x86_64 build needs nothing outside its architecture directories. The
designs that A6 found encoded for x86_64 are now stated for both
architectures: the thread local storage layout (`minios/dl.h`), the signal
frame contract between `arch_signal_setup_frame` and the libc restorer (a
return address on the stack on x86_64, x30 on aarch64), and the
`long double` format (`libc/src/ldouble.h`). The aarch64 port still lacks
the application processors (A8) and the tcc backend and package
repositories (A9). The kernel self-test `cpu` of x86 features and the
`kbd` case of the 8042 controller remain x86 specific.
