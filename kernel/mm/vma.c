#define KLOG_SUBSYS "vma"
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/swap.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <arch/cpu.h>
#include <arch/trap.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

static uint64_t vma_pte_flags(unsigned flags)
{
    uint64_t pte = PTE_P | PTE_U;
    if (flags & VM_WRITE)
        pte |= PTE_W;
    if (!(flags & VM_EXEC))
        pte |= PTE_NX;
    return pte;
}

struct vma *vma_find_locked(struct vmspace *vm, uintptr_t addr)
{
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (addr < v->start)
            break;
        if (addr < v->end)
            return v;
    }
    return NULL;
}

static int vma_add_locked(struct vmspace *vm, uintptr_t start, uintptr_t end, unsigned flags)
{
    struct list_head *pos = vm->vmas.next;
    while (pos != &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->start >= end)
            break;
        if (v->end > start)
            return -EEXIST;
        pos = pos->next;
    }
    struct vma *n = kmalloc(sizeof *n);
    if (!n)
        return -ENOMEM;
    n->start = start;
    n->end = end;
    n->flags = flags;
    list_add_tail(&n->link, pos);
    return 0;
}

int vma_add(struct vmspace *vm, uintptr_t start, uintptr_t end, unsigned flags)
{
    if (!IS_ALIGNED(start, PAGE_SIZE) || !IS_ALIGNED(end, PAGE_SIZE) || start >= end ||
        start < USER_BASE || end - 1 > USER_TOP)
        return -EINVAL;
    spin_lock(&vm->lock);
    int r = vma_add_locked(vm, start, end, flags);
    spin_unlock(&vm->lock);
    return r;
}

/* Map a fresh zero page at va with the region's protection. */
static int map_zero_page(struct vmspace *vm, uintptr_t va, unsigned flags)
{
    struct page *pg = pmm_alloc_page();
    if (!pg)
        return -ENOMEM;
    memset(P2V(page_to_phys(pg)), 0, PAGE_SIZE);
    uint64_t *entry;
    int r = paging_walk(vm->pml4_phys, va, true, &entry);
    if (r < 0) {
        pmm_free_page(pg);
        return r;
    }
    kassert(r == 1 && !(*entry & PTE_P));
    page_get(pg);
    *entry = page_to_phys(pg) | vma_pte_flags(flags);
    return 0;
}

int vma_populate(struct vmspace *vm, uintptr_t start, uintptr_t end)
{
    int r = 0;
    spin_lock(&vm->lock);
    for (uintptr_t va = ALIGN_DOWN(start, PAGE_SIZE); va < end && r == 0; va += PAGE_SIZE) {
        struct vma *v = vma_find_locked(vm, va);
        if (!v) {
            r = -EFAULT;
            break;
        }
        uint64_t *entry;
        int w = paging_walk(vm->pml4_phys, va, false, &entry);
        if (w == 1 && (*entry & PTE_P))
            continue;
        r = map_zero_page(vm, va, v->flags);
    }
    spin_unlock(&vm->lock);
    return r;
}

void vma_unmap_range_locked(struct vmspace *vm, uintptr_t start, uintptr_t end)
{
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *entry;
        if (paging_walk(vm->pml4_phys, va, false, &entry) != 1)
            continue;
        if (*entry & PTE_P) {
            if (pmm_is_ram(*entry & PTE_ADDR_MASK))
                page_put(phys_to_page(*entry & PTE_ADDR_MASK));
            *entry = 0;
        } else if (*entry & PTE_SWAPPED) {
            swap_free_slot(*entry >> 12);
            *entry = 0;
        }
    }
    tlb_flush_range(vm, start, end - start);
}

void vma_remove_all(struct vmspace *vm)
{
    spin_lock(&vm->lock);
    while (!list_empty(&vm->vmas)) {
        struct vma *v = list_first_entry(&vm->vmas, struct vma, link);
        vma_unmap_range_locked(vm, v->start, v->end);
        list_del(&v->link);
        kfree(v);
    }
    vm->brk_start = vm->brk = 0;
    spin_unlock(&vm->lock);
}

long vma_brk(struct vmspace *vm, intptr_t increment)
{
    spin_lock(&vm->lock);
    long old = (long)vm->brk;
    if (!vm->brk_start) {
        spin_unlock(&vm->lock);
        return -ENOMEM;
    }
    uintptr_t new_brk = ALIGN_UP(vm->brk + increment, PAGE_SIZE);
    struct vma *heap = vma_find_locked(vm, vm->brk_start);
    kassert(heap != NULL && heap->start == vm->brk_start);
    if (increment > 0) {
        /* The heap must not run into the next region. */
        struct vma *next = list_entry(heap->link.next, struct vma, link);
        if (new_brk < vm->brk || (heap->link.next != &vm->vmas && new_brk > next->start) ||
            new_brk - 1 > USER_TOP) {
            spin_unlock(&vm->lock);
            return -ENOMEM;
        }
        heap->end = new_brk;
    } else if (increment < 0) {
        if (new_brk < vm->brk_start + PAGE_SIZE)
            new_brk = vm->brk_start + PAGE_SIZE;
        vma_unmap_range_locked(vm, new_brk, heap->end);
        heap->end = new_brk;
    }
    vm->brk = new_brk;
    spin_unlock(&vm->lock);
    return old;
}

bool vma_range_ok(struct vmspace *vm, uintptr_t addr, size_t len, bool write)
{
    if (addr < USER_BASE || addr > USER_TOP || len > USER_TOP - addr + 1)
        return false;
    uintptr_t end = addr + len;
    bool ok = true;
    spin_lock(&vm->lock);
    uintptr_t va = addr;
    while (va < end) {
        struct vma *v = vma_find_locked(vm, va);
        if (!v || (write && !(v->flags & VM_WRITE))) {
            ok = false;
            break;
        }
        va = v->end;
    }
    spin_unlock(&vm->lock);
    return ok;
}

/* Copy on write: give the faulting address its own writable frame. */
static int do_cow_locked(struct vmspace *vm, uintptr_t va, uint64_t *entry)
{
    struct page *old = phys_to_page(*entry & PTE_ADDR_MASK);
    uint64_t flags = (*entry & PTE_FLAGS_MASK & ~PTE_COW) | PTE_W;
    if (__atomic_load_n(&old->refcount, __ATOMIC_SEQ_CST) == 1) {
        *entry = (*entry & PTE_ADDR_MASK) | flags;
    } else {
        struct page *pg = pmm_alloc_page();
        if (!pg)
            return -ENOMEM;
        memcpy(P2V(page_to_phys(pg)), P2V(page_to_phys(old)), PAGE_SIZE);
        page_get(pg);
        *entry = page_to_phys(pg) | flags;
        page_put(old);
    }
    tlb_flush_range(vm, va, PAGE_SIZE);
    return 0;
}

/* Install a fresh zero frame at va unless one appeared meanwhile. The
 * frame is allocated with the lock dropped so the allocation may wait for
 * kswapd. */
static bool fault_in_zero_page(struct vmspace *vm, uintptr_t va)
{
    spin_unlock(&vm->lock);
    struct page *pg = swap_alloc_user_frame();
    spin_lock(&vm->lock);
    if (!pg)
        return false;
    struct vma *v = vma_find_locked(vm, va);
    uint64_t *entry;
    int w = paging_walk(vm->pml4_phys, va, true, &entry);
    if (!v || w != 1 || (*entry & (PTE_P | PTE_SWAPPED))) {
        pmm_free_page(pg);
        return v != NULL && w == 1;
    }
    memset(P2V(page_to_phys(pg)), 0, PAGE_SIZE);
    page_get(pg);
    *entry = page_to_phys(pg) | vma_pte_flags(v->flags);
    return true;
}

bool vmm_handle_fault(struct trapframe *tf, uintptr_t addr)
{
    struct vmspace *vm = cpu_current()->vm;
    if (!vm || vm == &kernel_vmspace || addr > USER_TOP)
        return false;
    bool write = tf->error & 2;
    bool present = tf->error & 1;
    uintptr_t va = ALIGN_DOWN(addr, PAGE_SIZE);
    bool ok = false;

    spin_lock(&vm->lock);
    struct vma *v = vma_find_locked(vm, va);
    if (!v)
        goto out;
    if (write && !(v->flags & VM_WRITE))
        goto out;
    uint64_t *entry;
    int w = paging_walk(vm->pml4_phys, va, false, &entry);
    if (present) {
        if (w == 1 && (*entry & PTE_P) && write && (*entry & PTE_COW))
            ok = do_cow_locked(vm, va, entry) == 0;
    } else if (w == 1 && (*entry & PTE_SWAPPED)) {
        spin_unlock(&vm->lock);
        return swap_in_page(vm, va) == 0;
    } else {
        ok = fault_in_zero_page(vm, va);
    }
out:
    spin_unlock(&vm->lock);
    return ok;
}

/* Share every present page of the parent with the child. Writable pages
 * become read only and tagged COW in both spaces. */
static int share_level(struct vmspace *vm, uint64_t *src, uint64_t *dst, int level, uintptr_t base)
{
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t e = src[i];
        if (!(e & PTE_P))
            continue;
        uintptr_t va = base + ((uintptr_t)i << (12 + 9 * (level - 1)));
        if (level == 1) {
            if (!pmm_is_ram(e & PTE_ADDR_MASK)) {
                dst[i] = e;     /* device memory: shared, not counted */
                continue;
            }
            struct vma *v = vma_find_locked(vm, va);
            if (v && (v->flags & VM_SHARED)) {
                dst[i] = e;     /* shared object: both map the same frame */
                page_get(phys_to_page(e & PTE_ADDR_MASK));
                continue;
            }
            if (e & PTE_W)
                e = (e & ~PTE_W) | PTE_COW;
            src[i] = e;
            dst[i] = e;
            page_get(phys_to_page(e & PTE_ADDR_MASK));
        } else {
            uintptr_t table = paging_alloc_table();
            if (!table)
                return -ENOMEM;
            dst[i] = table | (e & PTE_FLAGS_MASK);
            int r = share_level(vm, P2V(e & PTE_ADDR_MASK), P2V(table), level - 1, va);
            if (r < 0)
                return r;
        }
    }
    return 0;
}

struct vmspace *vmspace_fork(struct vmspace *vm)
{
    /* Swapped pages are not present and would be skipped by the copy, so
     * bring everything in first and keep kswapd away until done. */
    spin_lock(&vm->lock);
    vm->pinned = true;
    spin_unlock(&vm->lock);
    if (swap_in_all(vm) < 0) {
        spin_lock(&vm->lock);
        vm->pinned = false;
        spin_unlock(&vm->lock);
        return NULL;
    }
    struct vmspace *child = vmspace_create();
    if (!child) {
        spin_lock(&vm->lock);
        vm->pinned = false;
        spin_unlock(&vm->lock);
        return NULL;
    }
    spin_lock(&vm->lock);
    struct list_head *pos;
    int r = 0;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        r = vma_add_locked(child, v->start, v->end, v->flags);
        if (r < 0)
            break;
    }
    if (r == 0) {
        uint64_t *src = P2V(vm->pml4_phys);
        uint64_t *dst = P2V(child->pml4_phys);
        for (int i = 0; i < PT_ENTRIES / 2 && r == 0; i++) {
            if (!(src[i] & PTE_P))
                continue;
            uintptr_t table = paging_alloc_table();
            if (!table) {
                r = -ENOMEM;
                break;
            }
            dst[i] = table | (src[i] & PTE_FLAGS_MASK);
            r = share_level(vm, P2V(src[i] & PTE_ADDR_MASK), P2V(table), 3, (uintptr_t)i << 39);
        }
        child->brk_start = vm->brk_start;
        child->brk = vm->brk;
    }
    /* Parent pages lost their write permission. */
    tlb_flush_range(vm, 0, USER_TOP + 1);
    vm->pinned = false;
    spin_unlock(&vm->lock);
    if (r < 0) {
        vma_remove_all(child);
        vmspace_destroy(child);
        return NULL;
    }
    return child;
}
