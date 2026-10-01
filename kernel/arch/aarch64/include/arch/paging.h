#pragma once
#include <kernel.h>
#include <mm/memlayout.h>

/* Page tables of aarch64 with 4 KiB granules and 48 bit addresses
 * (docs/design/arch.md, A2). The geometry and the functions are those of
 * the x86_64 header; the descriptor format follows the ARMv8 VMSA.
 *
 * Write permission and the dirty state use the scheme of the hardware
 * dirty bit management (FEAT_HAFDBS): PTE_WRITE (the DBM bit) marks an
 * entry writable, AP[2] (PTE_RDONLY) makes the hardware refuse writes, and
 * a writable entry with PTE_RDONLY clear is dirty. PTE_DIRTY_SW records
 * the dirty state while the entry is write protected. Where the processor does
 * not manage the access flag and the dirty state, the fault handler sets
 * them (A5). */

typedef uint64_t pte_t;

#define PT_LEVELS   4
#define PT_ENTRIES  512
#define PT_SHIFT(level)      (PAGE_SHIFT + 9 * ((level) - 1))
#define PT_LEVEL_SIZE(level) (1UL << PT_SHIFT(level))
#define PT_INDEX(va, level)  (((uintptr_t)(va) >> PT_SHIFT(level)) & (PT_ENTRIES - 1))
/* TTBR0 maps user space with the same layout as x86_64, so the user half
 * covers the first half of the TTBR0 root. The kernel uses its own root
 * in TTBR1. */
#define PT_ROOT_USER_ENTRIES (PT_ENTRIES / 2)

#define PAGE_2M     PT_LEVEL_SIZE(2)

/* Descriptor bits, private to the architecture. */
#define PTE_VALID       (1UL << 0)
#define PTE_TYPE_PAGE   (1UL << 1)      /* level 3 page or level 0-2 table; clear for a block */
#define PTE_ATTR(i)     ((uint64_t)(i) << 2)    /* MAIR_EL1 index */
#define PTE_ATTR_MASK   PTE_ATTR(7)
#define PTE_USER        (1UL << 6)      /* AP[1]: accessible from EL0 */
#define PTE_RDONLY      (1UL << 7)      /* AP[2]: read only */
#define PTE_SH_INNER    (3UL << 8)
#define PTE_AF          (1UL << 10)     /* access flag */
#define PTE_NG          (1UL << 11)     /* not global: tagged with the ASID */
#define PTE_WRITE       (1UL << 51)     /* DBM: writable */
#define PTE_PXN         (1UL << 53)
#define PTE_UXN         (1UL << 54)
#define PTE_DIRTY_SW    (1UL << 55)     /* software: dirty while write protected */
#define PTE_COW         (1UL << 56)     /* software: copy on write, see mm/vma.c */
#define PTE_SWAPPED     (1UL << 57)     /* software: not valid, bits 12+ contain the swap slot */
#define PTE_LAZYFREE    (1UL << 58)     /* software: MADV_FREE (M38) */
#define PTE_PROTNONE    (1UL << 59)     /* software: not valid, a frame is attached (M37) */
#define PTE_ADDR_MASK   0x0000fffffffff000UL

/* MAIR_EL1 indices programmed by the kernel. */
#define MAIR_IDX_NORMAL 0               /* write-back cacheable */
#define MAIR_IDX_DEVICE 1               /* Device-nGnRE */
#define MAIR_IDX_NC     2               /* normal non-cacheable, write combining */

/* ---- reading entries ---- */

static inline bool pte_present(pte_t e)
{
    return (e & PTE_VALID) != 0;
}

static inline bool pte_mapped(pte_t e)
{
    return (e & (PTE_VALID | PTE_PROTNONE)) != 0;
}

/* A level 1 to 3 table descriptor (levels 2 to PT_LEVELS in minios
 * numbering). */
static inline bool pte_is_table(pte_t e)
{
    return (e & (PTE_VALID | PTE_TYPE_PAGE)) == (PTE_VALID | PTE_TYPE_PAGE);
}

/* A 2 MiB block descriptor at level 2. */
static inline bool pte_is_block(pte_t e)
{
    return (e & (PTE_VALID | PTE_TYPE_PAGE)) == PTE_VALID;
}

static inline uintptr_t pte_addr(pte_t e)
{
    return e & PTE_ADDR_MASK;
}

static inline pte_t *pte_table(pte_t e)
{
    return P2V(pte_addr(e));
}

static inline bool pte_write(pte_t e)      { return (e & PTE_WRITE) != 0; }
static inline bool pte_user(pte_t e)       { return (e & PTE_USER) != 0; }
static inline bool pte_young(pte_t e)      { return (e & PTE_AF) != 0; }
static inline bool pte_dirty(pte_t e)
{
    return (e & PTE_DIRTY_SW) || ((e & PTE_WRITE) && !(e & PTE_RDONLY));
}
static inline bool pte_cow(pte_t e)        { return (e & PTE_COW) != 0; }
static inline bool pte_lazyfree(pte_t e)   { return (e & PTE_LAZYFREE) != 0; }
static inline bool pte_protnone(pte_t e)   { return (e & PTE_PROTNONE) != 0; }
static inline bool pte_swapped(pte_t e)    { return (e & PTE_SWAPPED) != 0; }

static inline uint64_t pte_swap_slot(pte_t e)
{
    return (e & PTE_ADDR_MASK) >> PAGE_SHIFT;
}

unsigned pte_vm_flags(pte_t e);

/* ---- building entries ---- */

pte_t pte_make(uintptr_t pa, unsigned vm_flags);

static inline pte_t pte_make_protnone(uintptr_t pa)
{
    return pa | PTE_USER | PTE_PROTNONE;
}

static inline pte_t pte_make_swap(uint64_t slot)
{
    return (slot << PAGE_SHIFT) | PTE_SWAPPED;
}

static inline pte_t pte_make_table(uintptr_t pa, bool user)
{
    (void)user;
    return pa | PTE_VALID | PTE_TYPE_PAGE;
}

static inline pte_t pte_set_addr(pte_t e, uintptr_t pa)
{
    return (e & ~PTE_ADDR_MASK) | pa;
}

static inline pte_t pte_mkblock(pte_t e)        { return e & ~PTE_TYPE_PAGE; }
static inline pte_t pte_block_to_page(pte_t e)  { return e | PTE_TYPE_PAGE; }

static inline pte_t pte_mkwrite(pte_t e)
{
    e |= PTE_WRITE;
    if (e & PTE_DIRTY_SW)
        e &= ~PTE_RDONLY;
    return e;
}

static inline pte_t pte_wrprotect(pte_t e)
{
    if (pte_dirty(e))
        e |= PTE_DIRTY_SW;
    return (e & ~PTE_WRITE) | PTE_RDONLY;
}

static inline pte_t pte_mkyoung(pte_t e)        { return e | PTE_AF; }
static inline pte_t pte_mkold(pte_t e)          { return e & ~PTE_AF; }

static inline pte_t pte_mkdirty(pte_t e)
{
    e |= PTE_DIRTY_SW;
    if (e & PTE_WRITE)
        e &= ~PTE_RDONLY;
    return e;
}

static inline pte_t pte_mkclean(pte_t e)
{
    return (e & ~PTE_DIRTY_SW) | PTE_RDONLY;
}

static inline pte_t pte_mkcow(pte_t e)          { return e | PTE_COW; }
static inline pte_t pte_clear_cow(pte_t e)      { return e & ~PTE_COW; }
static inline pte_t pte_mklazyfree(pte_t e)     { return e | PTE_LAZYFREE; }
static inline pte_t pte_clear_lazyfree(pte_t e) { return e & ~PTE_LAZYFREE; }

/* ---- tables ---- */

uintptr_t paging_alloc_table(void);
void paging_free_table(uintptr_t pa);
int paging_walk(uintptr_t root, uintptr_t va, bool create, pte_t **entry);
int paging_walk_preallocated(uintptr_t root, uintptr_t va,
                             const uintptr_t *tables, unsigned table_count,
                             unsigned *used, pte_t **entry);
int paging_pde(uintptr_t root, uintptr_t va, bool create, pte_t **entry);
int paging_map_large(uintptr_t root, uintptr_t va, uintptr_t pa, size_t size, unsigned vm_flags);
uintptr_t paging_init_kernel_root(void);
void paging_init_user_root(uintptr_t root, uintptr_t kernel_root);
void paging_free_user_tables(uintptr_t root);
/* Architecture part of paging_free_user_tables (mm/pgtable.c), called
 * before the tables are freed: on aarch64 the release of the ASID. */
void paging_release_user_root(uintptr_t root);

/* Load root as the user translation (TTBR0_EL1) of the calling CPU. */
void paging_load(uintptr_t root);
void paging_flush_page(uintptr_t va);
void paging_flush_user(void);
void paging_enable_features(void);
/* Make the instruction fetches of every CPU see the data written to the
 * frame at pa: clean the data cache to the point of unification and
 * invalidate the instruction caches, unless CTR_EL0 reports them
 * coherent. Called before a frame the kernel wrote is mapped executable. */
void paging_sync_icache(uintptr_t pa);
const char *paging_describe(void);
