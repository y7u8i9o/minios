#pragma once
#include <kernel.h>
#include <mm/memlayout.h>

/* Page tables (docs/design/arch.md, A2). Generic code walks the tables
 * through the geometry below and reads and builds entries only through
 * the pte_* functions; the bit layout is private to the architecture.
 *
 * The tables have PT_LEVELS levels of PT_ENTRIES entries. Level 1 entries
 * map 4 KiB pages, a level 2 entry is a table pointer or a 2 MiB block,
 * and levels 3 and up hold table pointers only. An entry that is not
 * present may still carry a frame (PROT_NONE) or a swap slot; the
 * software bits that mark these states are part of the entry format. */

typedef uint64_t pte_t;

#define PT_LEVELS   4
#define PT_ENTRIES  512
#define PT_SHIFT(level)      (PAGE_SHIFT + 9 * ((level) - 1))
#define PT_LEVEL_SIZE(level) (1UL << PT_SHIFT(level))
#define PT_INDEX(va, level)  (((uintptr_t)(va) >> PT_SHIFT(level)) & (PT_ENTRIES - 1))
/* Root entries that map user space; the rest of the root maps the kernel
 * and is shared by every address space (paging_init_user_root). */
#define PT_ROOT_USER_ENTRIES (PT_ENTRIES / 2)

#define PAGE_2M     PT_LEVEL_SIZE(2)

/* x86_64 entry bits, private to the architecture. */
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

/* ---- reading entries ---- */

/* The hardware translates through the entry. */
static inline bool pte_present(pte_t e)
{
    return (e & PTE_P) != 0;
}

/* A frame is attached: present, or kept for a PROT_NONE region. */
static inline bool pte_mapped(pte_t e)
{
    return (e & (PTE_P | PTE_PROTNONE)) != 0;
}

/* A level 2 to PT_LEVELS entry that points to the next table. */
static inline bool pte_is_table(pte_t e)
{
    return (e & (PTE_P | PTE_PS)) == PTE_P;
}

/* A level 2 entry that maps a 2 MiB block. */
static inline bool pte_is_block(pte_t e)
{
    return (e & (PTE_P | PTE_PS)) == (PTE_P | PTE_PS);
}

/* Physical address of the frame, block or next table. */
static inline uintptr_t pte_addr(pte_t e)
{
    return e & PTE_ADDR_MASK;
}

/* The next table of a table entry, through the direct map. */
static inline pte_t *pte_table(pte_t e)
{
    return P2V(pte_addr(e));
}

static inline bool pte_write(pte_t e)      { return (e & PTE_W) != 0; }
static inline bool pte_user(pte_t e)       { return (e & PTE_U) != 0; }
static inline bool pte_young(pte_t e)      { return (e & PTE_A) != 0; }
static inline bool pte_dirty(pte_t e)      { return (e & PTE_D) != 0; }
static inline bool pte_cow(pte_t e)        { return (e & PTE_COW) != 0; }
static inline bool pte_lazyfree(pte_t e)   { return (e & PTE_LAZYFREE) != 0; }
static inline bool pte_protnone(pte_t e)   { return (e & PTE_PROTNONE) != 0; }
static inline bool pte_swapped(pte_t e)    { return (e & PTE_SWAPPED) != 0; }

static inline uint64_t pte_swap_slot(pte_t e)
{
    return e >> PAGE_SHIFT;
}

/* The VM_* protection, cache and scope flags of a present entry. */
unsigned pte_vm_flags(pte_t e);

/* ---- building entries ---- */

/* A present 4 KiB page entry for pa with the VM_* flags (VM_WRITE,
 * VM_EXEC, VM_USER, VM_NOCACHE, VM_WC, VM_GLOBAL; readable always). */
pte_t pte_make(uintptr_t pa, unsigned vm_flags);

/* A user entry that keeps the frame pa for a PROT_NONE region. */
static inline pte_t pte_make_protnone(uintptr_t pa)
{
    return pa | PTE_U | PTE_PROTNONE;
}

/* A non-present entry that records a swap slot. */
static inline pte_t pte_make_swap(uint64_t slot)
{
    return (slot << PAGE_SHIFT) | PTE_SWAPPED;
}

/* A table entry for the next table at pa. */
static inline pte_t pte_make_table(uintptr_t pa, bool user)
{
    return pa | PTE_P | PTE_W | (user ? PTE_U : 0);
}

/* The same entry with another frame or table. */
static inline pte_t pte_set_addr(pte_t e, uintptr_t pa)
{
    return (e & PTE_FLAGS_MASK) | pa;
}

/* A level 2 block entry with the attributes of the page entry e, and the
 * reverse. */
static inline pte_t pte_mkblock(pte_t e)        { return e | PTE_PS; }
static inline pte_t pte_block_to_page(pte_t e)  { return e & ~PTE_PS; }

static inline pte_t pte_mkwrite(pte_t e)        { return e | PTE_W; }
static inline pte_t pte_wrprotect(pte_t e)      { return e & ~PTE_W; }
static inline pte_t pte_mkyoung(pte_t e)        { return e | PTE_A; }
static inline pte_t pte_mkold(pte_t e)          { return e & ~PTE_A; }
static inline pte_t pte_mkdirty(pte_t e)        { return e | PTE_D; }
static inline pte_t pte_mkclean(pte_t e)        { return e & ~PTE_D; }
static inline pte_t pte_mkcow(pte_t e)          { return e | PTE_COW; }
static inline pte_t pte_clear_cow(pte_t e)      { return e & ~PTE_COW; }
static inline pte_t pte_mklazyfree(pte_t e)     { return e | PTE_LAZYFREE; }
static inline pte_t pte_clear_lazyfree(pte_t e) { return e & ~PTE_LAZYFREE; }

/* ---- tables ---- */

/* Allocate a zeroed page table page. Returns its physical address or 0. */
uintptr_t paging_alloc_table(void);
void paging_free_table(uintptr_t pa);

/* Find the entry that maps va. With create, intermediate tables are
 * allocated. Returns the level of the entry found (1 for a 4 KiB PTE, 2 for a
 * 2 MiB block), 0 if the walk hit a non present entry without create, or
 * -ENOMEM. *entry receives a pointer to the entry through the HHDM. */
int paging_walk(uintptr_t root, uintptr_t va, bool create, pte_t **entry);
/* Walk while consuming already allocated and zeroed table pages instead of
 * allocating under the address-space lock. *used receives the number of
 * entries consumed from tables. */
int paging_walk_preallocated(uintptr_t root, uintptr_t va,
                             const uintptr_t *tables, unsigned table_count,
                             unsigned *used, pte_t **entry);

/* Find the level 2 entry covering va, creating the upper tables with
 * create. Returns 1 with *entry set, 0 when a table is missing without
 * create, or -ENOMEM. The entry may be a 2 MiB block, a table pointer or
 * empty. */
int paging_pde(uintptr_t root, uintptr_t va, bool create, pte_t **entry);

/* Map a range with 2 MiB blocks wherever alignment allows, with the VM_*
 * flags. Kernel use only. */
int paging_map_large(uintptr_t root, uintptr_t va, uintptr_t pa, size_t size, unsigned vm_flags);

/* Allocate the kernel root table and the tables of the kernel half that
 * every address space shares. Returns the root or 0. */
uintptr_t paging_init_kernel_root(void);
/* Prepare the root of a new user address space: on x86_64 the kernel half
 * of the kernel root is copied into it. */
void paging_init_user_root(uintptr_t root, uintptr_t kernel_root);

/* Free every table page reachable from the user half of root, then the
 * root itself. Mapped frames are not freed. */
void paging_free_user_tables(uintptr_t root);
/* Architecture part of paging_free_user_tables (mm/pgtable.c), called
 * before the tables are freed: on aarch64 the release of the ASID. */
void paging_release_user_root(uintptr_t root);

/* Load root as the translation of the calling CPU. */
static inline void paging_load(uintptr_t root)
{
    __asm__ volatile("movq %0, %%cr3" : : "r"(root) : "memory");
}

/* Drop the translation of one page on the calling CPU. */
static inline void paging_flush_page(uintptr_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/* Drop every user translation on the calling CPU. Reloading CR3 keeps the
 * global kernel mappings. */
static inline void paging_flush_user(void)
{
    uintptr_t root;
    __asm__ volatile("movq %%cr3, %0" : "=r"(root));
    __asm__ volatile("movq %0, %%cr3" : : "r"(root) : "memory");
}

/* Enable the MMU features the kernel depends on (NX, global pages, write
 * combining) on the calling CPU. */
void paging_enable_features(void);
/* Text appended to the page table log line of vmm_init. */
const char *paging_describe(void);
