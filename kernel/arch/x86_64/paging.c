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

uintptr_t paging_alloc_table(void)
{
    struct page *pg = pmm_alloc_page();
    if (!pg)
        return 0;
    uintptr_t pa = page_to_phys(pg);
    memset(P2V(pa), 0, PAGE_SIZE);
    return pa;
}

void paging_free_table(uintptr_t pa)
{
    pmm_free_page(phys_to_page(pa));
}

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

/* Descend one level, creating the next table if asked. */
static pte_t *next_level(pte_t *entry, bool create, bool user)
{
    if (*entry & PTE_P) {
        if (*entry & PTE_PS)
            return NULL;
        return pte_table(*entry);
    }
    if (!create)
        return NULL;
    uintptr_t pa = paging_alloc_table();
    if (!pa)
        return NULL;
    *entry = pte_make_table(pa, user);
    return P2V(pa);
}

int paging_walk(uintptr_t root, uintptr_t va, bool create, pte_t **entry)
{
    bool user = va <= USER_TOP;
    pte_t *pml4 = P2V(root);
    pte_t *pdpt, *pd, *pt;

    pdpt = next_level(&pml4[PT_INDEX(va, 4)], create, user);
    if (!pdpt)
        return create ? -ENOMEM : 0;
    pd = next_level(&pdpt[PT_INDEX(va, 3)], create, user);
    if (!pd)
        return create ? -ENOMEM : 0;
    pte_t *pde = &pd[PT_INDEX(va, 2)];
    if ((*pde & PTE_P) && (*pde & PTE_PS)) {
        *entry = pde;
        return 2;
    }
    pt = next_level(pde, create, user);
    if (!pt)
        return create ? -ENOMEM : 0;
    *entry = &pt[PT_INDEX(va, 1)];
    return 1;
}

static pte_t *next_level_preallocated(pte_t *entry, bool user,
                                      const uintptr_t *tables,
                                      unsigned table_count, unsigned *used)
{
    if (*entry & PTE_P) {
        if (*entry & PTE_PS)
            return NULL;
        return pte_table(*entry);
    }
    if (*used >= table_count)
        return NULL;
    uintptr_t pa = tables[(*used)++];
    *entry = pte_make_table(pa, user);
    return P2V(pa);
}

int paging_walk_preallocated(uintptr_t root, uintptr_t va,
                             const uintptr_t *tables, unsigned table_count,
                             unsigned *used, pte_t **entry)
{
    bool user = va <= USER_TOP;
    pte_t *pml4 = P2V(root);
    *used = 0;
    pte_t *pdpt = next_level_preallocated(&pml4[PT_INDEX(va, 4)], user,
                                             tables, table_count, used);
    if (!pdpt)
        return -ENOMEM;
    pte_t *pd = next_level_preallocated(&pdpt[PT_INDEX(va, 3)], user,
                                           tables, table_count, used);
    if (!pd)
        return -ENOMEM;
    pte_t *pde = &pd[PT_INDEX(va, 2)];
    if ((*pde & PTE_P) && (*pde & PTE_PS)) {
        *entry = pde;
        return 2;
    }
    pte_t *pt = next_level_preallocated(pde, user, tables,
                                           table_count, used);
    if (!pt)
        return -ENOMEM;
    *entry = &pt[PT_INDEX(va, 1)];
    return 1;
}

int paging_pde(uintptr_t root, uintptr_t va, bool create, pte_t **entry)
{
    bool user = va <= USER_TOP;
    pte_t *pml4 = P2V(root);
    pte_t *pdpt = next_level(&pml4[PT_INDEX(va, 4)], create, user);
    if (!pdpt)
        return create ? -ENOMEM : 0;
    pte_t *pd = next_level(&pdpt[PT_INDEX(va, 3)], create, user);
    if (!pd)
        return create ? -ENOMEM : 0;
    *entry = &pd[PT_INDEX(va, 2)];
    return 1;
}

int paging_map_large(uintptr_t root, uintptr_t va, uintptr_t pa, size_t size, unsigned vm_flags)
{
    pte_t flags = pte_make(0, vm_flags);
    uintptr_t end = va + size;
    while (va < end) {
        pte_t *entry;
        bool big = IS_ALIGNED(va, PAGE_2M) && IS_ALIGNED(pa, PAGE_2M) && end - va >= PAGE_2M;
        if (big) {
            pte_t *pml4 = P2V(root);
            pte_t *pdpt = next_level(&pml4[PT_INDEX(va, 4)], true, false);
            if (!pdpt)
                return -ENOMEM;
            pte_t *pd = next_level(&pdpt[PT_INDEX(va, 3)], true, false);
            if (!pd)
                return -ENOMEM;
            pd[PT_INDEX(va, 2)] = pa | flags | PTE_PS;
            va += PAGE_2M;
            pa += PAGE_2M;
            continue;
        }
        int r = paging_walk(root, va, true, &entry);
        if (r < 0)
            return r;
        *entry = pa | flags;
        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
    return 0;
}

static void free_level(pte_t *table, int level)
{
    for (int i = 0; i < PT_ENTRIES; i++) {
        pte_t e = table[i];
        if (!pte_is_table(e))
            continue;
        if (level > 1)
            free_level(pte_table(e), level - 1);
        paging_free_table(pte_addr(e));
    }
}

uintptr_t paging_init_kernel_root(void)
{
    uintptr_t root = paging_alloc_table();
    if (!root)
        return 0;
    /* Populate every kernel half root entry now so user spaces can copy
     * them once and stay in sync with all later kernel mappings. */
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

void paging_free_user_tables(uintptr_t root)
{
    pte_t *pml4 = P2V(root);
    for (int i = 0; i < PT_ROOT_USER_ENTRIES; i++) {
        pte_t e = pml4[i];
        if (!(e & PTE_P))
            continue;
        free_level(pte_table(e), 3);
        paging_free_table(pte_addr(e));
        pml4[i] = 0;
    }
    paging_free_table(root);
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
     * 0 (WB), 2 (UC-) and 3 (UC) keep their defaults. */
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
