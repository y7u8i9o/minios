#include <tests/ktest.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/vma.h>
#include <arch/paging.h>
#include <mm/memlayout.h>
#include <arch/cpu.h>
#include <lib/string.h>
#include <console.h>
#include <errno.h>

/* M4: mappings in the kernel space and in a fresh user space behave, and
 * nothing leaks from page table allocation. */
static void vmm_round(void)
{
    volatile int stack_probe = 0;

    /* Kernel space: map a frame at a fresh virtual address. */
    struct page *pg = pmm_alloc_page();
    ktest_assert(pg != NULL, "page alloc failed");
    uintptr_t pa = page_to_phys(pg);
    uintptr_t va = KHEAP_BASE;
    ktest_assert(vmm_map(&kernel_vmspace, va, pa, PAGE_SIZE, VM_KERNEL_RW) == 0, "kernel map failed");
    ktest_assert(vmm_map(&kernel_vmspace, va, pa, PAGE_SIZE, VM_KERNEL_RW) == -EEXIST, "double map allowed");

    volatile uint64_t *through_map = (volatile uint64_t *)va;
    volatile uint64_t *through_hhdm = P2V(pa);
    *through_map = 0x1234567890abcdefUL;
    ktest_assert(*through_hhdm == 0x1234567890abcdefUL, "write through mapping not visible");

    uintptr_t tpa;
    unsigned tflags;
    ktest_assert(vmm_translate(&kernel_vmspace, va + 0x123, &tpa, &tflags), "translate failed");
    ktest_assert(tpa == pa + 0x123, "translate gave %lx, expected %lx", tpa, pa + 0x123);
    ktest_assert((tflags & (VM_WRITE | VM_EXEC | VM_USER)) == VM_WRITE, "flags %x", tflags);

    ktest_assert(vmm_protect(&kernel_vmspace, va, PAGE_SIZE, VM_READ | VM_GLOBAL) == 0, "protect failed");
    ktest_assert(vmm_translate(&kernel_vmspace, va, NULL, &tflags), "translate after protect failed");
    ktest_assert(!(tflags & VM_WRITE), "still writable after protect");
    ktest_assert(*through_map == 0x1234567890abcdefUL, "read after protect failed");

    ktest_assert(vmm_unmap(&kernel_vmspace, va, PAGE_SIZE) == 0, "unmap failed");
    ktest_assert(!vmm_translate(&kernel_vmspace, va, NULL, NULL), "still mapped after unmap");

    /* HHDM and kernel image translations. */
    ktest_assert(vmm_translate(&kernel_vmspace, (uintptr_t)through_hhdm, &tpa, NULL) && tpa == pa,
                 "hhdm translation wrong");
    ktest_assert(vmm_translate(&kernel_vmspace, (uintptr_t)vmm_round, NULL, &tflags) &&
                 (tflags & VM_EXEC) && !(tflags & VM_WRITE), "text mapping flags %x", tflags);
    ktest_assert(vmm_translate(&kernel_vmspace, (uintptr_t)&stack_probe, NULL, &tflags) &&
                 !(tflags & VM_EXEC) && (tflags & VM_WRITE), "stack mapping flags %x", tflags);

    /* Boot stack guard page is unmapped. */
    extern char boot_stack[];
    ktest_assert(!vmm_translate(&kernel_vmspace, (uintptr_t)boot_stack, NULL, NULL), "boot stack guard mapped");

    /* A user address space sharing the kernel half. */
    struct vmspace *vm = vmspace_create();
    ktest_assert(vm != NULL, "vmspace_create failed");
    ktest_assert(vmm_map(vm, 0x400000, pa, PAGE_SIZE, VM_READ | VM_WRITE | VM_USER) == 0, "user map failed");
    ktest_assert(vmm_map(vm, KSTACK_BASE, pa, PAGE_SIZE, VM_KERNEL_RW) == -EINVAL, "kernel address accepted in user space");
    vmspace_activate(vm);
    ktest_assert(vmspace_current() == vm, "vmspace_current wrong");
    volatile uint64_t *user = (volatile uint64_t *)0x400000;
    ktest_assert(*user == 0x1234567890abcdefUL, "user mapping reads wrong value");
    *user = 42;
    ktest_assert(*through_hhdm == 42, "user write not visible");
    vmspace_activate(&kernel_vmspace);
    ktest_assert(vmm_translate(vm, 0x400000, &tpa, &tflags) && tpa == pa && (tflags & VM_USER), "user translate wrong");
    ktest_assert(vmm_unmap(vm, 0x400000, PAGE_SIZE) == 0, "user unmap failed");
    vmspace_destroy(vm);

    /* Kernel stacks with guard pages. */
    void *top = kstack_alloc();
    ktest_assert(top != NULL, "kstack_alloc failed");
    uintptr_t t = (uintptr_t)top;
    ktest_assert(vmm_translate(&kernel_vmspace, t - PAGE_SIZE, NULL, NULL), "stack top page unmapped");
    ktest_assert(vmm_translate(&kernel_vmspace, t - KSTACK_SIZE, NULL, NULL), "stack bottom page unmapped");
    ktest_assert(!vmm_translate(&kernel_vmspace, t - KSTACK_SIZE - PAGE_SIZE, NULL, NULL), "guard page mapped");
    memset((void *)(t - KSTACK_SIZE), 0xa5, KSTACK_SIZE);
    void *top2 = kstack_alloc();
    ktest_assert(top2 != NULL && top2 != top, "second kstack_alloc failed");
    kstack_free(top);
    kstack_free(top2);
    ktest_assert(!vmm_translate(&kernel_vmspace, t - PAGE_SIZE, NULL, NULL), "stack still mapped after free");

    /* MMIO mapping of a RAM page as a stand in for a device. */
    volatile uint64_t *mmio = vmm_map_mmio(pa + 0x10, 8, VM_READ | VM_WRITE | VM_NOCACHE);
    ktest_assert(mmio != NULL, "mmio map failed");
    ktest_assert(vmm_translate(&kernel_vmspace, (uintptr_t)mmio, &tpa, &tflags) && tpa == pa + 0x10 &&
                 (tflags & VM_NOCACHE), "mmio translate wrong");
    through_hhdm[2] = 77;
    ktest_assert(*mmio == 77, "mmio read wrong");

    pmm_free_page(pg);
}

static void test_vmm(void)
{
    /* The first round creates the kernel page tables of the heap, stack and
     * MMIO regions, which are never freed, and the first slabs of the
     * caches it uses. The test runs before most of the kernel has used
     * them (KTEST_MEMORY), so only the second round is measured. */
    vmm_round();
    struct pmm_stats before, after;
    pmm_get_stats(&before);
    vmm_round();
    pmm_get_stats(&after);
    ktest_assert(after.free_pages >= before.free_pages, "leaked %lu pages",
                 before.free_pages - after.free_pages);
    kprintf("vmm: %lu pages before, %lu after\n", before.free_pages, after.free_pages);
}
KTEST_DEFINE_STAGE("vmm", test_vmm, KTEST_MEMORY);

static void test_munmap_tables(void)
{
    struct vmspace *vm = vmspace_create();
    ktest_assert(vm != NULL, "munmap test address space");
    uintptr_t base = 0x400000;
    unsigned flags = VM_READ | VM_WRITE | VM_USER | VM_MMAP;
    ktest_assert(vma_add(vm, base, base + 3 * PAGE_SIZE, flags) == 0 &&
                 vma_populate(vm, base, base + 3 * PAGE_SIZE) == 0, "populate three pages");
    ktest_assert(vma_mprotect(vm, base + PAGE_SIZE, PAGE_SIZE, 0) == 0, "protect middle page");
    ktest_assert(vma_munmap(vm, base, PAGE_SIZE) == 0 &&
                 vma_munmap(vm, base + 2 * PAGE_SIZE, PAGE_SIZE) == 0, "unmap outside pages");
    pte_t *entry;
    ktest_assert(paging_walk(vm->pt_root, base + PAGE_SIZE, false, &entry) == 1 &&
                 pte_protnone(*entry), "PROT_NONE page lost its table");
    ktest_assert(vma_munmap(vm, base + PAGE_SIZE, PAGE_SIZE) == 0, "unmap middle page");
    ktest_assert(paging_pde(vm->pt_root, base, false, &entry) == 1 && *entry == 0,
                 "empty leaf table retained");
    ktest_assert(vma_munmap(vm, (uintptr_t)-PAGE_SIZE, 2 * PAGE_SIZE) == -EINVAL,
                 "overflowing kernel range accepted");
    ktest_assert(vma_munmap(vm, base, (size_t)-PAGE_SIZE) == -EINVAL,
                 "overflowing user range accepted");

    /* A sparse range spans absent level 4 and level 3 entries. */
    uintptr_t far = 1UL << 46;
    ktest_assert(vma_add(vm, far, far + PAGE_SIZE, flags) == 0 &&
                 vma_populate(vm, far, far + PAGE_SIZE) == 0, "populate distant page");
    ktest_assert(vma_munmap(vm, USER_BASE, USER_TOP - USER_BASE + 1) == 0,
                 "sparse whole-user-range unmap");
    ktest_assert(paging_pde(vm->pt_root, far, false, &entry) == 1 && *entry == 0,
                 "distant empty table retained");
    vmspace_destroy(vm);
    kprintf("munmap_tables: protected entries retained, empty tables reclaimed, sparse ranges checked\n");
}
KTEST_DEFINE_STAGE("munmap_tables", test_munmap_tables, KTEST_MEMORY);
