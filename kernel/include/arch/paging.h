#pragma once
#include <kernel.h>

/* x86_64 4 level page table entries. */
#define PTE_P       (1UL << 0)
#define PTE_W       (1UL << 1)
#define PTE_U       (1UL << 2)
#define PTE_PWT     (1UL << 3)
#define PTE_PCD     (1UL << 4)
#define PTE_A       (1UL << 5)
#define PTE_D       (1UL << 6)
#define PTE_PS      (1UL << 7)
#define PTE_G       (1UL << 8)
#define PTE_COW     (1UL << 9)    /* software: copy on write, see mm/vma.c */
#define PTE_SWAPPED (1UL << 10)   /* software: not present, bits 12+ hold the swap slot, see mm/swap.c */
#define PTE_LAZYFREE (1UL << 52)  /* software: MADV_FREE, kswapd may discard the frame while the dirty bit stays clear (M38) */
#define PTE_PROTNONE (1UL << 53)  /* software: not present, a frame is attached but the region is PROT_NONE (M37) */
#define PTE_NX      (1UL << 63)
#define PTE_ADDR_MASK 0x000ffffffffff000UL
#define PTE_FLAGS_MASK (~PTE_ADDR_MASK)

#define PT_ENTRIES  512
#define PAGE_2M     (1UL << 21)

#define PML4_INDEX(va) (((uintptr_t)(va) >> 39) & 0x1ff)
#define PDPT_INDEX(va) (((uintptr_t)(va) >> 30) & 0x1ff)
#define PD_INDEX(va)   (((uintptr_t)(va) >> 21) & 0x1ff)
#define PT_INDEX(va)   (((uintptr_t)(va) >> 12) & 0x1ff)

/* Allocate a zeroed page table page. Returns its physical address or 0. */
uintptr_t paging_alloc_table(void);
void paging_free_table(uintptr_t pa);

/* Find the entry that maps va. With create, intermediate tables are
 * allocated. Returns the level of the entry found (1 for a 4 KiB PTE, 2 for a
 * 2 MiB PDE), 0 if the walk hit a non present entry without create, or
 * -ENOMEM. *entry receives a pointer to the entry through the HHDM. */
int paging_walk(uintptr_t pml4_phys, uintptr_t va, bool create, uint64_t **entry);

/* Find the page directory entry covering va, creating the upper tables
 * with create. Returns 1 with *entry set, 0 when a table is missing
 * without create, or -ENOMEM. The entry may be a 2 MiB page (PTE_PS), a
 * table pointer or empty. */
int paging_pde(uintptr_t pml4_phys, uintptr_t va, bool create, uint64_t **entry);

/* Map a range with 2 MiB pages wherever alignment allows. Kernel use only. */
int paging_map_large(uintptr_t pml4_phys, uintptr_t va, uintptr_t pa, size_t size, uint64_t flags);

/* Free every table page reachable from the lower half of pml4, then the
 * PML4 itself. Mapped frames are not freed. */
void paging_free_user_tables(uintptr_t pml4_phys);

static inline void paging_load(uintptr_t pml4_phys)
{
    __asm__ volatile("movq %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static inline void paging_invlpg(uintptr_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

void paging_enable_features(void);
