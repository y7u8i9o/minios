# Virtual memory (M4)

## Layout

| Range | Use |
|---|---|
| `0x0000000000001000` to `0x00007fffffffffff` | user space |
| `hhdm_offset` (from Limine, `0xffff800000000000`) | direct map of physical memory |
| `0xffffffa000000000`, 64 GiB | device mappings (`vmm_map_mmio`) |
| `0xffffffc000000000` | kernel stacks, 4096 slots of guard page plus 16 KiB |
| `0xffffffd000000000` | reserved for a future vmalloc style allocator |
| `0xffffffff80000000` | kernel image |

## Page tables

`arch/x86_64/paging.c` holds the entry level operations: table allocation
from the buddy allocator, `paging_walk` (returns the level 1 PTE or a 2 MiB
PDE), `paging_walk_preallocated` for consuming table pages prepared before a
space lock is taken, `paging_map_large` for the kernel's bulk mappings and
`paging_free_user_tables`. `paging_enable_features` turns on `EFER.NXE`,
`CR0.WP` (so kernel writes honour read only pages, needed for copy on write)
and `CR4.PGE`, and programs PAT entry 1 as write combining, which `PTE_PWT`
then selects for framebuffer mappings.

`vmm_init` builds the kernel PML4: the direct map covers the usable,
bootloader reclaimable, kernel, ACPI and framebuffer regions with 2 MiB pages
wherever aligned, the framebuffer with write combining. The kernel image is
mapped from the linker symbols with text RX, rodata R and data RW, all
global. All 256 upper half PML4 entries are populated with PDPTs at init, so
`vmspace_create` copies them once and every later kernel mapping is visible
to every address space without bookkeeping.

After the switch, the first page of the boot stack becomes a guard page and
the bootloader reclaimable memory is handed to the buddy allocator. To make
this possible `boot_init` copies the memory map, the command line and the
framebuffer description out of Limine's memory into `struct bootinfo`.

## Address spaces

`struct vmspace` carries the PML4 physical address, a spinlock protecting its
page tables and an M46 per-CPU resident-page counter. The kernel instance is
`kernel_vmspace` and its lock is `kvm_lock`. `vmspace_create` allocates a new
PML4 with the kernel half shared. `vmspace_destroy` frees the lower half
tables and the PML4; the mapped frames belong to the process layer (M9).
`vmspace_activate` loads CR3 and records the space in `cpu_current()->vm`.

`vmm_map`, `vmm_unmap`, `vmm_protect` and `vmm_translate` work on 4 KiB
pages. Flags are `VM_READ`, `VM_WRITE`, `VM_EXEC`, `VM_USER`, `VM_NOCACHE`,
`VM_WC` and `VM_GLOBAL`. Mapping an already mapped page returns `-EEXIST`.
A user space rejects kernel addresses with `-EINVAL`.

`tlb_flush_range(vm, va, size)` is called with `vm->lock` held by every
unmap and protect. It uses `invlpg` for kernel addresses (global pages
survive CR3 reloads) and for short user ranges, and a CR3 reload for long
user ranges. Since M18 the function is in `mm/tlb.c`. `smp.md` describes how the flush reaches the other CPUs whose TLBs may contain the translations, by shootdown IPIs on x86_64 and by broadcast invalidation on aarch64 (A8).

M46 moves user-frame and populate-time page-table allocation and zeroing
outside `vmspace.lock`. The locked phase revalidates the VMA, consumes any
preallocated table pages it needs and publishes the final PTE. Multi-page
unmap clears entries under the lock and performs one range shootdown after
the batch rather than one shootdown per page. Whole-space removal is used only
immediately before `vmspace_destroy`; its mandatory TLB-drop round therefore
serves as the teardown shootdown as well.

Kernel stacks come from `kstack_alloc`, which takes a slot from a bitmap,
maps `KSTACK_SIZE` bytes above an unmapped guard page and returns the top.

## Page faults

`trap_dispatch` passes `#PF` to `vmm_handle_fault` first. Until copy on
write and demand paging exist it returns false and the trap handler prints
the faulting address, error code decoding and instruction pointer, then
panics with a backtrace.

A write fault on a present page whose entry already carries `PTE_W` is
treated as resolved. It occurs when two threads of one process fault on
the same copy on write page at the same time: the first one copies the
frame and sends the shootdown, the second one takes the fault from a
stale TLB entry, services the shootdown while spinning on the space
lock, and then finds the entry writable. Before this rule the second
thread was killed with `SIGSEGV`.

## Test

`tests/cases/vmm` maps, translates, protects and unmaps a page in the
kernel space, checks the flags of the text, data and direct map regions,
verifies the boot stack guard, exercises a user space (map, activate, access,
destroy), allocates and frees guarded kernel stacks, maps MMIO and checks
that no pages leak apart from the retained kernel page tables.
