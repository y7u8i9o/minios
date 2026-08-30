#pragma once
#include <kernel.h>
#include <lib/list.h>

#define PMM_MAX_ORDER 10    /* 4 KiB .. 4 MiB blocks */

#define PG_RESERVED  (1u << 0)   /* not managed by the allocator */
#define PG_FREE      (1u << 1)   /* head of a free block, order is valid */
#define PG_SLAB      (1u << 2)   /* part of a slab, order is the slab order */
#define PG_LARGE     (1u << 3)   /* part of a large kmalloc block */

/* One entry per physical page frame. lru, flags and order are protected by
 * pmm_lock. refcount counts user mappings of an allocated frame and is
 * modified only with the atomic helpers below. */
struct page {
    struct list_head lru;
    uint16_t flags;
    uint8_t order;
    uint8_t pad;
    uint32_t refcount;
};

struct pmm_stats {
    uint64_t total_pages;   /* pages handed to the allocator */
    uint64_t free_pages;
    uint64_t reserved_pages;
};

void pmm_init(void);
/* Free the bootloader reclaimable regions. Called once the kernel no longer
 * uses any Limine provided structure or page table. */
void pmm_reclaim_bootloader(void);

/* Allocate a block of 2^order pages. Returns NULL on exhaustion. */
struct page *pmm_alloc(unsigned order);
void pmm_free(struct page *page, unsigned order);
struct page *pmm_alloc_page(void);
void pmm_free_page(struct page *page);

uintptr_t page_to_phys(const struct page *page);
struct page *phys_to_page(uintptr_t pa);
uint64_t page_to_pfn(const struct page *page);
struct page *pfn_to_page(uint64_t pfn);

/* Reference counting for frames shared between address spaces (COW). A
 * freshly allocated frame has refcount 0; the first mapping sets it to 1. */
static inline void page_get(struct page *page)
{
    __atomic_add_fetch(&page->refcount, 1, __ATOMIC_SEQ_CST);
}
/* Drop a reference, freeing the single page frame when it reaches zero.
 * Returns true if the frame was freed. */
bool page_put(struct page *page);

void pmm_get_stats(struct pmm_stats *out);
/* True if pa lies inside memory described by struct page. */
bool pmm_is_ram(uintptr_t pa);
/* Copy the per order free block counts into out[PMM_MAX_ORDER + 1]. */
void pmm_get_free_counts(uint64_t *out);
void pmm_dump_stats(void);
