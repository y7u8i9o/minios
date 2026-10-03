# Platform dependent behaviour

The kernel has one architecture, x86_64, but it runs on two processor
vendors and under several execution environments: QEMU's TCG, KVM on
Linux, HVF on macOS, and bare hardware. TCG implements the Intel
behaviour of every instruction, so a kernel that only ever ran under TCG
has never been tested against AMD semantics or against a processor with
caches that TCG does not model. This document lists every place where
the kernel depends on such behaviour and the rule for adding another.
The boundary between generic and architecture code, which the aarch64 port
builds on, is described in `arch.md`.

## Rules

1. `cpu_identify` (`arch/x86_64/cpu.c`) is the only function that
   executes `cpuid`. It fills `struct cpu_features` once on the boot CPU,
   before any feature is enabled, and logs vendor, family, model, the
   hypervisor signature and the feature list. Every other decision reads
   `cpu_features`.
2. A value that must be correct on both vendors is chosen so that no
   runtime branch is needed. `STAR[63:48]` carries RPL 3 itself because
   Intel ORs 3 into the `sysret` selectors and AMD does not; with a
   neutral value there is nothing to detect.
3. A control register or MSR bit is enabled only after its CPUID bit has
   been checked, and a missing feature stops the boot with a panic that
   names it (`paging_enable_features`, `fpu_init_cpu`). A `#GP` at boot
   with no message is what an unchecked `wrmsr` produces.
4. A difference that cannot be removed by a neutral encoding becomes a
   field of `cpu_features`, and this document lists its consumers.
5. `make test-kvm` runs the cases in `KVM_CASES` under KVM. Every change
   to `arch/x86_64`, `mm`, `sched` or `syscall` is run through it on the
   Linux machine before it is committed.

## Vendor differences

| Item | Intel | AMD | Kernel |
|------|-------|-----|--------|
| `sysret` selectors | `STAR[63:48]+8 \| 3`, `+16 \| 3` | `STAR[63:48]+8`, `+16 \| 3` (no OR into SS) | base 0x13, `syscall/table.c` |
| `sysret` SS attributes | reloaded | cached attributes unchanged | no effect on 64 bit only user programs; documented in `console.md` |
| null selector into FS or GS | base cleared | base retained | GS base is rewritten with `wrmsr` after every segment load (`smp.c`, `gdt.c`) |
| family and model encoding | extended fields for family 6 and 15 | extended fields for family 15 | `cpu_identify` |

## Environment differences

| Item | TCG | KVM, HVF, hardware | Kernel |
|------|-----|--------------------|--------|
| `invlpg` and `mov cr3` | flush the software TLB completely | flush what the architecture promises; global pages survive `mov cr3` | `tlb_flush_range` uses `invlpg` for kernel ranges and reloads CR3 only for user ranges |
| paging structure caches | none | a processor may cache a page directory entry it never used architecturally (speculative walks) | a shootdown goes to every CPU in `vmspace.cpu_mask`, never only to CPUs that touched the range; tables are freed after the flush (`vma_munmap`) |
| concurrent faults | rare | two CPUs fault on one copy on write page at the same time | a write fault on an entry that is already writable is resolved (`vma_resolve_fault`) |
| thread start | the child of `fork` starts after the parent yields | the child starts on another CPU as soon as it is queued | `proc_fork` completes the child's FPU image and FS base before `sched_add` |
| TSC and APIC timer rate | derived from host time, slow instruction rate | real rates (3 GHz, 1 GHz bus) | both are calibrated against the PIT; nothing assumes a rate |
| interrupts from user mode | few, because instructions are slow | at the full 1000 Hz | the `iretq` path is exercised early; the panic dump prints the frame it consumed |
| console daemon | rarely mid update at a panic | halted mid update by `smp_halt_others` | `fbcon_write` finishes a pending scroll before drawing |

## Diagnostics

A kernel mode fault prints, after the register dump, the five word return
frame at the stack pointer, the live selectors, CR0, CR4, EFER, GDTR,
IDTR, the GDT entries, both GS base MSRs, the `syscall` MSRs, the TSS
`rsp0` and the values the CPU pushed at the last entry from user mode on
that CPU (`trap_dump_extra`, `console.md`). The boot log's `[I cpu]`
lines identify the processor and the hypervisor, so a report from another
machine states which environment produced it.

## Test

`cpu` (kernel): checks that `cpu_features` names a known vendor with a
complete vendor string, that every feature the kernel enables without
asking is present, that CR4 and EFER carry the enabled bits, and that
`STAR[63:48]` has RPL 3. `make test-kvm` runs it together with `boot`,
`exception`, `fork`, `signals`, `smp`, `smp_user`, `vmm` and `sched`
under KVM.
