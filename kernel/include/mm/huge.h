#pragma once
#include <kernel.h>
#include <arch/paging.h>

struct vmspace;
struct vma;
struct page;

/* 2 MiB pages for anonymous regions flagged VM_HUGE (M39). A huge frame
 * is an order 9 buddy block mapped by a level 2 block entry (pte_is_block);
 * the head page's refcount counts the spaces mapping it. A huge entry
 * never straddles a region boundary and a block is never mapped both as a
 * 2 MiB page and as 4 KiB pages: every operation on part of a huge page
 * splits it first (huge_split_at), which requires exclusive ownership and
 * copies the block otherwise. See docs/design/hugepages.md. */

#define HUGE_ORDER 9

/* Try to satisfy a not present fault at va inside v with a huge page.
 * Called with vm->lock held, returns with it held. 1: mapped, 0: not
 * possible here (the caller falls back to a small page), -errno. */
int huge_fault_locked(struct vmspace *vm, struct vma *v, uintptr_t va);
/* Copy on write of the huge page at pde (present, pte_cow). Caller holds
 * vm->lock. */
int huge_cow_locked(struct vmspace *vm, uintptr_t va, pte_t *pde);
/* If addr lies strictly inside a huge page, replace that page by 512 small
 * entries so [addr, ...) can be handled page by page. Caller holds
 * vm->lock. 0 or -ENOMEM. */
int huge_split_at(struct vmspace *vm, uintptr_t addr);
/* Split every huge page overlapping [start, end). Caller holds vm->lock. */
int huge_split_range(struct vmspace *vm, uintptr_t start, uintptr_t end);
/* Fork: make the huge entry at src copy on write and let dst share it.
 * Caller holds the parent's vm->lock. */
void huge_share_locked(pte_t *src, pte_t *dst);
/* Drop one mapping reference of a huge frame, freeing the block when it
 * was the last. */
void huge_put(struct page *head);
/* Release the huge page mapped by the block entry pde and clear the
 * entry. Caller holds vm->lock; the TLB flush is the caller's. */
void huge_unmap_locked(pte_t *pde);

struct huge_stats {
    uint64_t mapped;        /* huge entries present right now */
    uint64_t splits;        /* huge pages broken into small ones */
    uint64_t fallbacks;     /* faults that found no 2 MiB frame */
};
void huge_get_stats(struct huge_stats *out);
