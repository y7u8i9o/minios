#include <arch/paging.h>
#include <arch/fpu.h>
#include <arch/cpu.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <mm/vmm.h>
#include <lib/string.h>
#include <errno.h>
#include <debug/panic.h>

#define CR0_WP   (1UL << 16)
#define CR4_PGE  (1UL << 7)
#define EFER_NXE (1UL << 11)
#define MSR_PAT  0x277

pte_t pte_make(uintptr_t pa, unsigned flags)
{
    pte_t pte = pa | PTE_P;
    if (flags & VM_WRITE)
        pte |= PTE_W;
    if (flags & VM_USER)
        pte |= PTE_U;
    if (!(flags & VM_EXEC))
        pte |= PTE_NX;
    if (flags & VM_NOCACHE)
        pte |= PTE_PCD | PTE_PWT;
    else if (flags & VM_WC)
        pte |= PTE_PWT;
    if (flags & VM_GLOBAL)
        pte |= PTE_G;
    return pte;
}

unsigned pte_vm_flags(pte_t pte)
{
    unsigned flags = VM_READ;
    if (pte & PTE_W)
        flags |= VM_WRITE;
    if (pte & PTE_U)
        flags |= VM_USER;
    if (!(pte & PTE_NX))
        flags |= VM_EXEC;
    if ((pte & (PTE_PCD | PTE_PWT)) == (PTE_PCD | PTE_PWT))
        flags |= VM_NOCACHE;
    else if (pte & PTE_PWT)
        flags |= VM_WC;
    if (pte & PTE_G)
        flags |= VM_GLOBAL;
    return flags;
}

uintptr_t paging_init_kernel_root(void)
{
    uintptr_t root = paging_alloc_table();
    if (!root)
        return 0;
    /* Populate every kernel half root entry now so user spaces can copy
     * them once and remain in sync with all later kernel mappings. */
    pte_t *table = P2V(root);
    for (int i = PT_ROOT_USER_ENTRIES; i < PT_ENTRIES; i++) {
        uintptr_t pdpt = paging_alloc_table();
        if (!pdpt)
            return 0;
        table[i] = pte_make_table(pdpt, false);
    }
    return root;
}

void paging_init_user_root(uintptr_t root, uintptr_t kernel_root)
{
    /* Share the kernel half. Every kernel root entry was populated by
     * paging_init_kernel_root, so later kernel mappings never need to
     * touch user spaces. */
    pte_t *src = P2V(kernel_root);
    pte_t *dst = P2V(root);
    memcpy(&dst[PT_ROOT_USER_ENTRIES], &src[PT_ROOT_USER_ENTRIES],
           (PT_ENTRIES - PT_ROOT_USER_ENTRIES) * sizeof(pte_t));
}

void paging_release_user_root(struct vmspace *vm)
{
    (void)vm;
}

void paging_load(struct vmspace *vm)
{
    __asm__ volatile("movq %0, %%cr3" : : "r"(vm->pt_root) : "memory");
}

void paging_flush_range(struct vmspace *vm, bool kernel, bool active, uintptr_t va, size_t size)
{
    (void)vm;
    if (!active)
        return;
    if (kernel || size <= 64 * PAGE_SIZE) {
        for (size_t off = 0; off < size; off += PAGE_SIZE)
            __asm__ volatile("invlpg (%0)" : : "r"(va + off) : "memory");
        return;
    }
    uintptr_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
}

const char *paging_describe(void)
{
    return cpu_features.pdpe1gb ? " with 1 GiB pages" : "";
}

void paging_enable_features(void)
{
    /* The bits below fault with #GP on a processor that lacks them; a
     * message names the missing feature instead. */
    if (!cpu_features.nx)
        panic("cpu: execute disable (NX) is required");
    if (!cpu_features.pge)
        panic("cpu: global pages (PGE) are required");
    if (!cpu_features.pat)
        panic("cpu: the page attribute table (PAT) is required");
    fpu_init_cpu();
    /* NX for non executable mappings. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);

    /* PAT entry 1 becomes write combining, selected by PTE_PWT. Entries
     * 0 (WB), 2 (UC-) and 3 (UC) retain their defaults. */
    uint64_t pat = rdmsr(MSR_PAT);
    pat = (pat & ~(0xffUL << 8)) | (0x01UL << 8);
    wrmsr(MSR_PAT, pat);

    uint64_t cr0, cr4;
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    cr0 |= CR0_WP;      /* kernel honours read only pages, needed for COW */
    cr4 |= CR4_PGE;     /* global kernel mappings survive CR3 loads */
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");
}
