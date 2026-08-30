#include <arch/paging.h>
#include <arch/fpu.h>
#include <arch/cpu.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <mm/vmm.h>
#include <lib/string.h>
#include <errno.h>

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

static uint64_t *table_of(uint64_t entry)
{
    return P2V(entry & PTE_ADDR_MASK);
}

/* Descend one level, creating the next table if asked. */
static uint64_t *next_level(uint64_t *entry, bool create, bool user)
{
    if (*entry & PTE_P) {
        if (*entry & PTE_PS)
            return NULL;
        return table_of(*entry);
    }
    if (!create)
        return NULL;
    uintptr_t pa = paging_alloc_table();
    if (!pa)
        return NULL;
    *entry = pa | PTE_P | PTE_W | (user ? PTE_U : 0);
    return P2V(pa);
}

int paging_walk(uintptr_t pml4_phys, uintptr_t va, bool create, uint64_t **entry)
{
    bool user = va <= USER_TOP;
    uint64_t *pml4 = P2V(pml4_phys);
    uint64_t *pdpt, *pd, *pt;

    pdpt = next_level(&pml4[PML4_INDEX(va)], create, user);
    if (!pdpt)
        return create ? -ENOMEM : 0;
    pd = next_level(&pdpt[PDPT_INDEX(va)], create, user);
    if (!pd)
        return create ? -ENOMEM : 0;
    uint64_t *pde = &pd[PD_INDEX(va)];
    if ((*pde & PTE_P) && (*pde & PTE_PS)) {
        *entry = pde;
        return 2;
    }
    pt = next_level(pde, create, user);
    if (!pt)
        return create ? -ENOMEM : 0;
    *entry = &pt[PT_INDEX(va)];
    return 1;
}

int paging_map_large(uintptr_t pml4_phys, uintptr_t va, uintptr_t pa, size_t size, uint64_t flags)
{
    uintptr_t end = va + size;
    while (va < end) {
        uint64_t *entry;
        bool big = IS_ALIGNED(va, PAGE_2M) && IS_ALIGNED(pa, PAGE_2M) && end - va >= PAGE_2M;
        if (big) {
            uint64_t *pml4 = P2V(pml4_phys);
            uint64_t *pdpt = next_level(&pml4[PML4_INDEX(va)], true, false);
            if (!pdpt)
                return -ENOMEM;
            uint64_t *pd = next_level(&pdpt[PDPT_INDEX(va)], true, false);
            if (!pd)
                return -ENOMEM;
            pd[PD_INDEX(va)] = pa | flags | PTE_P | PTE_PS;
            va += PAGE_2M;
            pa += PAGE_2M;
            continue;
        }
        int r = paging_walk(pml4_phys, va, true, &entry);
        if (r < 0)
            return r;
        *entry = pa | flags | PTE_P;
        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
    return 0;
}

static void free_level(uint64_t *table, int level)
{
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t e = table[i];
        if (!(e & PTE_P) || (e & PTE_PS))
            continue;
        if (level > 1)
            free_level(table_of(e), level - 1);
        paging_free_table(e & PTE_ADDR_MASK);
    }
}

void paging_free_user_tables(uintptr_t pml4_phys)
{
    uint64_t *pml4 = P2V(pml4_phys);
    for (int i = 0; i < PT_ENTRIES / 2; i++) {
        uint64_t e = pml4[i];
        if (!(e & PTE_P))
            continue;
        free_level(table_of(e), 3);
        paging_free_table(e & PTE_ADDR_MASK);
        pml4[i] = 0;
    }
    paging_free_table(pml4_phys);
}

void paging_enable_features(void)
{
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
