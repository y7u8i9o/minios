# Architecture interface

The kernel is divided into generic code and architecture code. The
architecture code of x86_64 is in `kernel/arch/x86_64/`, and its headers
are in `kernel/arch/x86_64/include/arch/`. The build selects the directory
with the `ARCH` variable of `toolchain.mk` (default `x86_64`): `kernel/Makefile`
compiles `arch/$(ARCH)` and adds `arch/$(ARCH)/include` to the include path,
so generic code includes `<arch/...>` without naming the architecture. The
linker script is `arch/$(ARCH)/linker.ld`.

Milestone A0 of the aarch64 port (`docs/plan/arm64.md`) introduced the
interface described here for the CPU and execution state: saved registers,
system call entry, signal frames, thread state, interrupt state, barriers,
power control and page fault decoding. Generic code uses these operations
and does not name an x86 register, instruction or MSR for them. A second
architecture implements the same headers with the same names.

Milestones A1 to A3 extend the interface to the platform devices, the MMU
and the user ABI. Until then generic code still uses `struct cpu`,
`cpu_current`, the LAPIC and IOAPIC calls, the PS/2, CMOS, serial, PCI and
timer drivers, the page table entry bits of `<arch/paging.h>` and the Limine
boot information directly. Section 9 lists what remains.

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
wakeup in between (`sti; hlt`). `push_cli` and `pop_cli` are not renamed
and remain the nesting interface used by spinlocks. The user space profiler
recognises lock primitives by these symbol names (`lockstat.md`), so they
are not renamed even though "cli" is an x86 term. Interrupt disabling is
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

## 7. Power control (`<arch/power.h>`)

`platform_power_off` and `platform_reboot` are called by the `reboot` system
call after the orderly shutdown sequence (`signals.md`). The x86_64
implementations are described in `console.md`.

## 8. Test

The kernel self-test `arch` (`kernel/tests/test_arch.c`, case
`tests/cases/arch`) checks the behaviour that generic callers rely on:
- the frame accessors on a frame built by `arch_frame_init_user`, the
  agreement between `frame_set_arg0` and `SYSARG0`, and the decoding of two
  page fault error codes;
- the nesting of `push_cli` and `pop_cli` and of `arch_irq_save` and
  `arch_irq_restore`;
- that `arch_set_tls` loads the register for the calling thread;
- that the `e_machine` field of `/bin/init` equals `ARCH_ELF_MACHINE`.

The behaviour of the moved code is covered by the existing cases of the
subsystems that use it: `fork`, `libc`, `signals`, `fpu`, `pthreads`,
`dynlink`, `smp`, `smp_user`, `lockfree`, `sched`, `exception`,
`backtrace`, `profile`, `vmm`, `swap`, `shutdown` and `blk`.

## 9. Dependencies that remain

A1 moves the per CPU state (`struct cpu`, `cpu_current`, `cpu_by_id`), the
interrupt vectors and `irq_register` with x86 vector numbers,
`lapic_send_ipi` and the MSI composition in `drivers/pci.c` behind
interfaces. It also moves the serial port, debug exit, CMOS RTC, PS/2 and
PCI configuration port drivers, the TSC clocksource and the LAPIC tick
(including the `read_rflags` and `hlt` wait in `drivers/timer.c`), and the
Limine structures in `bootinfo` that the physical allocator, the
framebuffer and the initrd use.

A2 moves the x86 page table entry bits, the open-coded four level walks in
`mm/` and the address layout in `memlayout.h` and `vmm.h`. A3 moves the
libc and loader assembly, the relocation types, `minios/simd.h` and the user
compiler flags. The kernel self-tests of x86 features, `cpu` and
`exception`, remain x86 specific.
