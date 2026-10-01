#define KLOG_SUBSYS "huge"
#include <mm/huge.h>
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* Counters, atomic updates. */
static struct huge_stats stats;

void huge_get_stats(struct huge_stats *out)
{
    out->mapped = __atomic_load_n(&stats.mapped, __ATOMIC_RELAXED);
    out->splits = __atomic_load_n(&stats.splits, __ATOMIC_RELAXED);
    out->fallbacks = __atomic_load_n(&stats.fallbacks, __ATOMIC_RELAXED);
}

void huge_put(struct page *head)
{
    uint32_t old = __atomic_fetch_sub(&head->refcount, 1, __ATOMIC_SEQ_CST);
    kassert(old > 0);
    if (old == 1)
        pmm_free(head, HUGE_ORDER);
}

void huge_unmap_locked(pte_t *pde)
{
    kassert(pte_is_block(*pde));
    struct page *head = phys_to_page(pte_addr(*pde));
    *pde = 0;
    huge_put(head);
    __atomic_fetch_sub(&stats.mapped, 1, __ATOMIC_RELAXED);
}

/* Allocate and zero a 2 MiB block. */
static struct page *alloc_huge_zeroed(void)
{
    struct page *pg = pmm_alloc(HUGE_ORDER);
    if (!pg)
        return NULL;
    memset(P2V(page_to_phys(pg)), 0, PAGE_2M);
    return pg;
}

int huge_fault_locked(struct vmspace *vm, struct vma *v, uintptr_t va)
{
    uintptr_t block = ALIGN_DOWN(va, PAGE_2M);
    if (block < v->start || block + PAGE_2M > v->end || (v->flags & (VM_FILE | VM_SHARED | VM_DEVICE)))
        return 0;
    pte_t *pde;
    int r = paging_pde(vm->pt_root, va, false, &pde);
    if (r == 1 && pte_present(*pde)) {
        if (pte_is_block(*pde))
            return 0;
        /* A page table covers the block, left behind by earlier small
         * pages. It only stands in the way while it is empty. */
        pte_t *table = pte_table(*pde);
        for (int i = 0; i < PT_ENTRIES; i++)
            if (table[i])
                return 0;
        paging_free_table(pte_addr(*pde));
        *pde = 0;
    }
    unsigned flags = v->flags;
    spin_unlock(&vm->lock);
    struct page *pg = alloc_huge_zeroed();
    spin_lock(&vm->lock);
    if (!pg) {
        __atomic_fetch_add(&stats.fallbacks, 1, __ATOMIC_RELAXED);
        return 0;
    }
    /* The space may have changed while the lock was dropped. */
    struct vma *cur = vma_find_locked(vm, va);
    r = paging_pde(vm->pt_root, va, true, &pde);
    if (r < 0 || cur != v || block < cur->start || block + PAGE_2M > cur->end || !(cur->flags & VM_HUGE) ||
        pte_present(*pde)) {
        pmm_free(pg, HUGE_ORDER);
        return r < 0 ? r : 0;
    }
    page_get(pg);
    *pde = pte_mkblock(vma_make_pte(page_to_phys(pg), flags));
    percpu_counter_add(&vm->resident, PT_ENTRIES);
    tlb_flush_range(vm, block, PAGE_2M);    /* a freed empty table may be cached by the page walker */
    __atomic_fetch_add(&stats.mapped, 1, __ATOMIC_RELAXED);
    return 1;
}

/* Replace the huge entry pde covering block by a page table. With an
 * exclusively owned block the small entries point into it; a shared block
 * is copied into small frames first. Caller holds vm->lock. */
static int split_locked(struct vmspace *vm, uintptr_t block, pte_t *pde)
{
    pte_t e = *pde;
    kassert(pte_is_block(e));
    struct page *head = phys_to_page(pte_addr(e));
    pte_t small = pte_block_to_page(e);
    uintptr_t pt = paging_alloc_table();
    if (!pt)
        return -ENOMEM;
    pte_t *table = P2V(pt);
    if (__atomic_load_n(&head->refcount, __ATOMIC_SEQ_CST) == 1) {
        pmm_split_block(head, HUGE_ORDER);
        for (int i = 0; i < PT_ENTRIES; i++) {
            struct page *pg = head + i;
            if (i)
                page_get(pg);   /* the head already carries its mapping reference */
            table[i] = pte_set_addr(small, page_to_phys(pg));
        }
    } else {
        /* Shared after fork: the copy is exclusively ours, so it may be
         * writable again where the region allows it. */
        struct vma *v = vma_find_locked(vm, block);
        pte_t own = pte_wrprotect(pte_clear_cow(small));
        if (v && (v->flags & VM_WRITE))
            own = pte_mkwrite(own);
        for (int i = 0; i < PT_ENTRIES; i++) {
            struct page *pg = pmm_alloc_page();
            if (!pg) {
                for (int j = 0; j < i; j++)
                    page_put(phys_to_page(pte_addr(table[j])));
                paging_free_table(pt);
                return -ENOMEM;
            }
            memcpy(P2V(page_to_phys(pg)), (char *)P2V(page_to_phys(head)) + (size_t)i * PAGE_SIZE, PAGE_SIZE);
            page_get(pg);
            table[i] = pte_set_addr(own, page_to_phys(pg));
        }
        huge_put(head);
    }
    *pde = pte_make_table(pt, true);
    tlb_flush_range(vm, block, PAGE_2M);
    __atomic_fetch_sub(&stats.mapped, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&stats.splits, 1, __ATOMIC_RELAXED);
    return 0;
}

int huge_split_at(struct vmspace *vm, uintptr_t addr)
{
    if (IS_ALIGNED(addr, PAGE_2M) || addr > USER_TOP)
        return 0;
    pte_t *pde;
    if (paging_pde(vm->pt_root, addr, false, &pde) != 1 || !pte_is_block(*pde))
        return 0;
    return split_locked(vm, ALIGN_DOWN(addr, PAGE_2M), pde);
}

int huge_split_range(struct vmspace *vm, uintptr_t start, uintptr_t end)
{
    for (uintptr_t block = ALIGN_DOWN(start, PAGE_2M); block < end; block += PAGE_2M) {
        pte_t *pde;
        if (paging_pde(vm->pt_root, block, false, &pde) != 1 || !pte_is_block(*pde))
            continue;
        int r = split_locked(vm, block, pde);
        if (r < 0)
            return r;
    }
    return 0;
}

void huge_share_locked(pte_t *src, pte_t *dst)
{
    pte_t e = pte_mkcow(pte_wrprotect(*src));
    *src = e;
    *dst = e;
    page_get(phys_to_page(pte_addr(e)));
    __atomic_fetch_add(&stats.mapped, 1, __ATOMIC_RELAXED);
}

int huge_cow_locked(struct vmspace *vm, uintptr_t va, pte_t *pde)
{
    uintptr_t block = ALIGN_DOWN(va, PAGE_2M);
    struct page *old = phys_to_page(pte_addr(*pde));
    pte_t writable = pte_mkwrite(pte_clear_cow(*pde));
    if (__atomic_load_n(&old->refcount, __ATOMIC_SEQ_CST) == 1) {
        *pde = writable;
    } else {
        struct page *pg = pmm_alloc(HUGE_ORDER);
        if (!pg) {
            /* No 2 MiB frame: copy into small pages instead. */
            __atomic_fetch_add(&stats.fallbacks, 1, __ATOMIC_RELAXED);
            return split_locked(vm, block, pde);
        }
        memcpy(P2V(page_to_phys(pg)), P2V(page_to_phys(old)), PAGE_2M);
        page_get(pg);
        *pde = pte_set_addr(writable, page_to_phys(pg));
        huge_put(old);
    }
    tlb_flush_range(vm, block, PAGE_2M);
    return 0;
}
