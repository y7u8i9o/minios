#pragma once
#include <kernel.h>
#include <lib/list.h>

#define PMM_MAX_ORDER 12    /* 4 KiB .. 16 MiB blocks (the GPU scanout buffer is one block) */

#define PG_RESERVED  (1u << 0)   /* not managed by the allocator */
#define PG_FREE      (1u << 1)   /* head of a free block, order is valid */
#define PG_SLAB      (1u << 2)   /* part of a slab, order is the slab order */
#define PG_LARGE     (1u << 3)   /* part of a large kmalloc block */
#define PG_CPUCACHE  (1u << 4)   /* free in one CPU's order-0 cache */

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
/* Return every per-CPU order-zero cache to the buddy allocator. The caller
 * must not have acquired an allocator lock. */
void pmm_reclaim_cpu_caches(void);
/* Turn an allocated block of 2^order pages into 2^order independent single
 * page allocations, each freeable with pmm_free_page or page_put (M39). */
void pmm_split_block(struct page *head, unsigned order);

/* A zeroed page for a device, through its direct map address, and its
 * physical address in *phys. Drivers give such pages to controllers as
 * rings, queues and transfer buffers. NULL on exhaustion. */
void *pmm_alloc_dma_page(uintptr_t *phys);
/* Free a page of pmm_alloc_dma_page by its direct map address. NULL is
 * ignored. */
void pmm_free_dma_page(void *va);

uintptr_t page_to_phys(const struct page *page);
struct page *phys_to_page(uintptr_t pa);
uint64_t page_to_pfn(const struct page *page);
struct page *pfn_to_page(uint64_t pfn);

/* Reference counting for frames shared between address spaces (COW). A
 * freshly allocated frame has refcount 0. The first mapping sets it to 1. */
static inline void page_get(struct page *page)
{
    __atomic_add_fetch(&page->refcount, 1, __ATOMIC_SEQ_CST);
}
/* Drop a reference, freeing the single page frame when it reaches zero.
 * Returns true if the frame was freed. */
bool page_put(struct page *page);

void pmm_get_stats(struct pmm_stats *out);
/* True if pa is a frame of the allocator, with a reference count: inside
 * the memory described by struct page and not reserved. */
bool pmm_is_ram(uintptr_t pa);
/* Copy the per order free block counts into out[PMM_MAX_ORDER + 1]. */
void pmm_get_free_counts(uint64_t *out);
void pmm_dump_stats(void);

/* A source of pages under memory pressure (V4 of the 0.6.0 release): the
 * balloon driver with VIRTIO_BALLOON_F_DEFLATE_ON_OOM. release returns up
 * to pages pages to the allocator and returns their number. The function
 * runs in any context, also with spinlocks acquired, but never with
 * pmm_lock. It must not sleep and must not allocate memory. */
void pmm_set_pressure_source(size_t (*release)(size_t pages));
/* Asks the pressure source for pages. Returns the number of pages that it
 * returned to the allocator, 0 without a source. */
size_t pmm_release_pressure(size_t pages);
/* Changes total_pages by pages. A balloon removes its pages from the
 * total while the host has them. */
void pmm_adjust_total(int64_t pages);
