#pragma once
#include <kernel.h>

struct vmspace;
struct page;

/* Swap: anonymous user pages are written to slots of a swap block device
 * (vdb) when free memory runs low. A page table entry of a swapped page
 * is not present and records the slot number (pte_make_swap,
 * pte_swap_slot in <arch/paging.h>). See docs/design/swap.md. */

/* Attach the swap device if present. Called once block devices exist. */
void swap_init(void);
/* Start kswapd. Called once the scheduler runs. */
void swap_start_daemon(void);
bool swap_enabled(void);

uint64_t swap_alloc_slot(void);
void swap_free_slot(uint64_t slot);

/* Allocate a frame for user data, waiting for kswapd to reclaim memory
 * when none is free. Must not be called with a spinlock held. Returns
 * NULL when memory cannot be reclaimed. */
struct page *swap_alloc_user_frame(void);
/* Bring the swapped page at va of vm back into memory. vm->lock must not
 * be held. Returns 0, or -errno. */
int swap_in_page(struct vmspace *vm, uintptr_t va);
/* Swap in every page of vm so it can be copied by fork. */
int swap_in_all(struct vmspace *vm);

struct swap_stats {
    uint64_t total_slots, free_slots;
    uint64_t swapped_out, swapped_in;
    uint64_t lazy_freed;        /* frames discarded through MADV_FREE (M38) */
};
void swap_get_stats(struct swap_stats *out);
/* Wait until no eviction batch is in flight, for exact accounting in tests. */
void swap_drain(void);
