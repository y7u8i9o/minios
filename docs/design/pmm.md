# Physical memory: buddy allocator (M3)

## Memory map

`pmm_init` walks the Limine memory map. Usable entries are page aligned by
the protocol. The highest usable address defines `pmm_max_pfn` and the size
of the `struct page` array (24 bytes per frame, 3 MiB for 512 MiB of RAM).
The array is carved from the start of the first usable region large enough
and is accessed through the higher half direct map (`P2V` in
`mm/memlayout.h`).

Bootloader reclaimable regions hold the Limine structures and the initial
page tables. They stay reserved until the kernel owns its page tables in M4.

## Data structures

    struct page { struct list_head lru; uint16_t flags; uint8_t order; ... }

`PG_RESERVED` marks frames outside the allocator. `PG_FREE` is set only on
the head of a free block, whose `order` field is then valid. Allocated blocks
keep their order in the head so `pmm_free` can check the caller's value.

Free blocks of each order 0 to 12 (4 KiB to 16 MiB) sit on one list. All
lists, the per order counts and `pmm_stats` are protected by `pmm_lock`.

## Algorithms

`free_block(pfn, order)`: while the order is below the maximum and the buddy
(`pfn ^ (1 << order)`) is the head of a free block of the same order, remove
the buddy and merge. Insert the result at the head of its list.

`pmm_alloc(order)`: find the first non empty list at or above the requested
order, take its head, and split it downwards, returning each upper half to
its list.

Initialization hands each usable region to `free_block` in the largest
aligned blocks that fit, so the free lists start in a coalesced state.

M46 places a 32-page order-zero cache in front of the buddy allocator on
each CPU. Allocation refills a local cache in batches of eight; a full cache
returns sixteen pages at a time. Each cache has its own lock, so the common
path never takes `pmm_lock` and maintenance code can safely drain a remote
CPU's cache. `pmm_reclaim_cpu_caches` returns all cached pages to the buddy
lists for high-order pressure and exact coalescing checks. Reported free-page
and order-zero counts include the per-CPU caches.

## Interface

    struct page *pmm_alloc(unsigned order);
    void pmm_free(struct page *page, unsigned order);
    struct page *pmm_alloc_page(void);
    void pmm_free_page(struct page *page);
    void pmm_reclaim_cpu_caches(void);
    uintptr_t page_to_phys(const struct page *);
    struct page *phys_to_page(uintptr_t);
    void pmm_get_stats(struct pmm_stats *);
    void pmm_get_free_counts(uint64_t out[PMM_MAX_ORDER + 1]);
    void pmm_dump_stats(void);

## Test

`tests/cases/pmm` snapshots the free counts, then allocates 1024 single pages
and frees them in reverse and interleaved order, allocates and frees one
block of every order checking alignment, splits a 16 MiB block into sixteen
1 MiB pieces freed in scrambled order, and finally exhausts order 12 to
verify that `pmm_alloc` returns `NULL`. After each phase the free counts must
equal the snapshot after draining the per-CPU page caches, which proves
coalescing.
