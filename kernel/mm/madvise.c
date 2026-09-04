#define KLOG_SUBSYS "madvise"
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/swap.h>
#include <mm/slab.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <minios/abi.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* Every page of [addr, end) must belong to a region; device regions accept
 * no advice. Caller holds vm->lock. */
static int check_range_locked(struct vmspace *vm, uintptr_t addr, uintptr_t end)
{
    uintptr_t va = addr;
    while (va < end) {
        struct vma *v = vma_find_locked(vm, va);
        if (!v)
            return -ENOMEM;
        if (v->flags & VM_DEVICE)
            return -EINVAL;
        va = v->end;
    }
    return 0;
}

/* Split the regions so that addr and end fall on region boundaries and
 * set or clear flag bits on every region inside. Caller holds vm->lock.
 * required_absent names region flags that make the advice invalid. */
static int set_flags_locked(struct vmspace *vm, uintptr_t addr, uintptr_t end, unsigned set, unsigned clear,
                            unsigned required_absent)
{
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->end <= addr || v->start >= end)
            continue;
        if (v->flags & required_absent)
            return -EINVAL;
        if (((v->flags | set) & ~clear) == v->flags)
            continue;
        if (v->start < addr) {
            if (!vma_split_locked(vm, v, addr))
                return -ENOMEM;
            continue;               /* the tail is visited next */
        }
        if (v->end > end && !vma_split_locked(vm, v, end))
            return -ENOMEM;
        v->flags = (v->flags | set) & ~clear;
    }
    return 0;
}

/* MADV_DONTNEED: drop the frames and swap slots of the range. Shared file
 * pages stay in the mapping, their dirty bits are recorded on the way. */
static int dontneed_locked(struct vmspace *vm, uintptr_t addr, uintptr_t end)
{
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->end <= addr || v->start >= end)
            continue;
        vma_unmap_range_locked(vm, v, MAX(v->start, addr), MIN(v->end, end));
    }
    return 0;
}

/* MADV_FREE: tag present private anonymous pages so kswapd may discard
 * them while they stay clean; swapped pages are dropped right away. */
static int lazyfree_locked(struct vmspace *vm, uintptr_t addr, uintptr_t end)
{
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->end <= addr || v->start >= end)
            continue;
        if (v->flags & (VM_FILE | VM_SHARED))
            return -EINVAL;
    }
    for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
        uint64_t *entry;
        if (paging_walk(vm->pml4_phys, va, false, &entry) != 1)
            continue;
        uint64_t e = *entry;
        if (e & PTE_P) {
            *entry = (e & ~PTE_D) | PTE_LAZYFREE;
        } else if (e & PTE_SWAPPED) {
            swap_free_slot(e >> 12);
            *entry = 0;
        }
    }
    tlb_flush_range(vm, addr, end - addr);
    return 0;
}

/* MADV_WILLNEED: bring every page of the range in as a read would. */
static int willneed(struct vmspace *vm, uintptr_t addr, uintptr_t end)
{
    for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
        spin_lock(&vm->lock);
        struct vma *v = vma_find_locked(vm, va);
        if (!v || !(v->flags & VM_READ)) {
            spin_unlock(&vm->lock);
            if (!v)
                return -ENOMEM;
            continue;
        }
        uint64_t *entry;
        int w = paging_walk(vm->pml4_phys, va, false, &entry);
        bool mapped = w == 1 && (*entry & (PTE_P | PTE_PROTNONE));
        spin_unlock(&vm->lock);
        if (mapped || w == 2)
            continue;
        /* Beyond the end of a file the fault fails; that is not an error
         * for advice. */
        vma_resolve_fault(vm, va, false, false);
    }
    return 0;
}

long vma_madvise(struct vmspace *vm, uintptr_t addr, size_t len, int advice)
{
    if (!IS_ALIGNED(addr, PAGE_SIZE) || addr < USER_BASE || addr + len < addr || addr + len - 1 > USER_TOP)
        return -EINVAL;
    if (len == 0)
        return 0;
    uintptr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    int r;
    spin_lock(&vm->lock);
    r = check_range_locked(vm, addr, end);
    if (r < 0) {
        spin_unlock(&vm->lock);
        return r;
    }
    switch (advice) {
    case MADV_NORMAL:
        r = set_flags_locked(vm, addr, end, 0, VM_RANDOM | VM_SEQUENTIAL, 0);
        break;
    case MADV_RANDOM:
        r = set_flags_locked(vm, addr, end, VM_RANDOM, VM_SEQUENTIAL, 0);
        break;
    case MADV_SEQUENTIAL:
        r = set_flags_locked(vm, addr, end, VM_SEQUENTIAL, VM_RANDOM, 0);
        break;
    case MADV_DONTFORK:
        r = set_flags_locked(vm, addr, end, VM_DONTFORK, 0, 0);
        break;
    case MADV_DOFORK:
        r = set_flags_locked(vm, addr, end, 0, VM_DONTFORK, 0);
        break;
    case MADV_HUGEPAGE:
        r = set_flags_locked(vm, addr, end, VM_HUGE, 0, VM_FILE | VM_SHARED);
        break;
    case MADV_NOHUGEPAGE:
        r = set_flags_locked(vm, addr, end, 0, VM_HUGE, 0);
        break;
    case MADV_DONTNEED:
        r = dontneed_locked(vm, addr, end);
        break;
    case MADV_FREE:
        r = lazyfree_locked(vm, addr, end);
        break;
    case MADV_WILLNEED:
        spin_unlock(&vm->lock);
        return willneed(vm, addr, end);
    default:
        r = -EINVAL;
        break;
    }
    spin_unlock(&vm->lock);
    return r;
}
