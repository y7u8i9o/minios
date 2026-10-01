#define KLOG_SUBSYS "vma"
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/swap.h>
#include <mm/filemap.h>
#include <mm/huge.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <arch/cpu.h>
#include <arch/frame.h>
#include <fs/vfs.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>
#include <sync/rcu.h>

/* The forward link is the RCU publication edge.  Removed nodes retain
 * their links until their grace-period callback, so a reader already on a
 * node can continue to its successor safely. */
static void vma_link_before_rcu(struct list_head *n, struct list_head *next)
{
    struct list_head *prev = next->prev;
    n->next = next;
    n->prev = prev;
    next->prev = n;
    __atomic_store_n(&prev->next, n, __ATOMIC_RELEASE);
}

static void vma_link_after_rcu(struct list_head *n, struct list_head *prev)
{
    struct list_head *next = prev->next;
    n->next = next;
    n->prev = prev;
    next->prev = n;
    __atomic_store_n(&prev->next, n, __ATOMIC_RELEASE);
}

static void vma_unlink_rcu(struct list_head *n)
{
    struct list_head *prev = n->prev;
    struct list_head *next = n->next;
    next->prev = prev;
    __atomic_store_n(&prev->next, next, __ATOMIC_RELEASE);
}

uint64_t vma_pte_flags(unsigned flags)
{
    uint64_t pte = PTE_U;
    if (!(flags & VM_READ))
        return pte | PTE_PROTNONE;
    pte |= PTE_P;
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

/* Insert n into the sorted list. Caller holds vm->lock and has checked
 * that the range is free. */
static void vma_insert_locked(struct vmspace *vm, struct vma *n)
{
    struct list_head *pos = vm->vmas.next;
    while (pos != &vm->vmas && list_entry(pos, struct vma, link)->start < n->start)
        pos = pos->next;
    vma_link_before_rcu(&n->link, pos);
}

static int vma_add_locked(struct vmspace *vm, uintptr_t start, uintptr_t end, unsigned flags)
{
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->start >= end)
            break;
        if (v->end > start)
            return -EEXIST;
    }
    struct vma *n = kzalloc(sizeof *n);
    if (!n)
        return -ENOMEM;
    n->start = start;
    n->end = end;
    n->flags = flags;
    vma_insert_locked(vm, n);
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

struct vma *vma_split_locked(struct vmspace *vm, struct vma *v, uintptr_t addr)
{
    kassert(addr > v->start && addr < v->end && IS_ALIGNED(addr, PAGE_SIZE));
    /* A huge page never straddles a region boundary. */
    if (huge_split_at(vm, addr) < 0)
        return NULL;
    struct vma *tail = kzalloc(sizeof *tail);
    if (!tail)
        return NULL;
    tail->start = addr;
    tail->end = v->end;
    tail->flags = v->flags;
    if (v->flags & VM_FILE) {
        tail->file = v->file;
        file_ref(tail->file);
        tail->mapping = v->mapping;
        filemap_ref(tail->mapping);
        tail->offset = v->offset + (addr - v->start);
    }
    __atomic_store_n(&v->end, addr, __ATOMIC_RELEASE);
    vma_link_after_rcu(&tail->link, &v->link);
    return tail;
}

static void vma_free_rcu(struct rcu_head *head)
{
    struct vma *v = container_of(head, struct vma, rcu);
    if (v->file)
        file_put(v->file);
    if (v->mapping)
        filemap_put(v->mapping);
    kfree(v);
}

void vma_release(struct vma *v)
{
    rcu_call(&v->rcu, vma_free_rcu);
}

int vma_populate(struct vmspace *vm, uintptr_t start, uintptr_t end)
{
    int r = 0;
    for (uintptr_t va = ALIGN_DOWN(start, PAGE_SIZE); va < end && r == 0; va += PAGE_SIZE) {
        struct page *pg = pmm_alloc_page();
        if (!pg)
            return -ENOMEM;
        memset(P2V(page_to_phys(pg)), 0, PAGE_SIZE);
        uintptr_t tables[3];
        unsigned table_count = 0;
        while (table_count < ARRAY_SIZE(tables)) {
            uintptr_t pa = paging_alloc_table();
            if (!pa)
                break;
            tables[table_count++] = pa;
        }
        if (table_count != ARRAY_SIZE(tables)) {
            for (unsigned i = 0; i < table_count; i++)
                paging_free_table(tables[i]);
            pmm_free_page(pg);
            return -ENOMEM;
        }
        spin_lock(&vm->lock);
        struct vma *v = vma_find_locked(vm, va);
        unsigned tables_used = 0;
        if (!v || (v->flags & VM_FILE)) {
            r = -EFAULT;
        } else {
            uint64_t *entry;
            int w = paging_walk_preallocated(vm->pml4_phys, va, tables,
                                             table_count, &tables_used, &entry);
            if (w < 0)
                r = w;
            else if (w == 1 && !(*entry & (PTE_P | PTE_PROTNONE | PTE_SWAPPED))) {
                page_get(pg);
                *entry = page_to_phys(pg) | vma_pte_flags(v->flags);
                percpu_counter_inc(&vm->resident);
                pg = NULL;
            }
        }
        spin_unlock(&vm->lock);
        for (unsigned i = tables_used; i < table_count; i++)
            paging_free_table(tables[i]);
        if (pg)
            pmm_free_page(pg);
    }
    return r;
}

void vma_unmap_range_locked(struct vmspace *vm, struct vma *v, uintptr_t start, uintptr_t end)
{
    bool track_dirty = v && (v->flags & VM_FILE) && (v->flags & VM_SHARED);
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *entry;
        int w = paging_walk(vm->pml4_phys, va, false, &entry);
        if (w == 2) {
            /* Whole huge pages only: the callers split at the boundaries. */
            kassert(IS_ALIGNED(va, PAGE_2M) && end - va >= PAGE_2M);
            huge_unmap_locked(entry);
            percpu_counter_add(&vm->resident, -(int64_t)PT_ENTRIES);
            va += PAGE_2M - PAGE_SIZE;
            continue;
        }
        if (w != 1)
            continue;
        uint64_t e = *entry;
        if (e & (PTE_P | PTE_PROTNONE)) {
            if (pmm_is_ram(e & PTE_ADDR_MASK)) {
                if (track_dirty && (e & PTE_D))
                    filemap_mark_dirty(v->mapping, (v->offset >> PAGE_SHIFT) + ((va - v->start) >> PAGE_SHIFT));
                page_put(phys_to_page(e & PTE_ADDR_MASK));
                percpu_counter_dec(&vm->resident);
            }
            *entry = 0;
        } else if (e & PTE_SWAPPED) {
            swap_free_slot(e >> 12);
            *entry = 0;
        }
    }
}

static void release_list(struct list_head *dead)
{
    while (!list_empty(dead)) {
        struct vma *v = list_first_entry(dead, struct vma, reclaim_link);
        list_del(&v->reclaim_link);
        vma_release(v);
    }
}

void vma_remove_all(struct vmspace *vm)
{
    LIST_HEAD(dead);
    LIST_HEAD(jobs);
    spin_lock(&vm->lock);
    while (!list_empty(&vm->vmas)) {
        struct vma *v = list_first_entry(&vm->vmas, struct vma, link);
        vma_unmap_range_locked(vm, v, v->start, v->end);
        vma_queue_sync(&jobs, v, v->start, v->end);
        vma_unlink_rcu(&v->link);
        list_add_tail(&v->reclaim_link, &dead);
    }
    vm->brk_start = vm->brk = 0;
    spin_unlock(&vm->lock);
    vma_run_sync_jobs(&jobs);
    release_list(&dead);
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
        __atomic_store_n(&heap->end, new_brk, __ATOMIC_RELEASE);
    } else if (increment < 0) {
        if (new_brk < vm->brk_start + PAGE_SIZE)
            new_brk = vm->brk_start + PAGE_SIZE;
        if (huge_split_at(vm, new_brk) < 0) {
            spin_unlock(&vm->lock);
            return -ENOMEM;
        }
        vma_unmap_range_locked(vm, heap, new_brk, heap->end);
        tlb_flush_range(vm, new_brk, heap->end - new_brk);
        __atomic_store_n(&heap->end, new_brk, __ATOMIC_RELEASE);
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
    rcu_read_lock();
    uintptr_t va = addr;
    while (va < end) {
        struct vma *found = NULL;
        struct list_head *pos = __atomic_load_n(&vm->vmas.next, __ATOMIC_ACQUIRE);
        while (pos != &vm->vmas) {
            struct vma *v = list_entry(pos, struct vma, link);
            uintptr_t vstart = __atomic_load_n(&v->start, __ATOMIC_ACQUIRE);
            uintptr_t vend = __atomic_load_n(&v->end, __ATOMIC_ACQUIRE);
            if (va < vstart)
                break;
            if (va < vend) {
                found = v;
                break;
            }
            pos = __atomic_load_n(&pos->next, __ATOMIC_ACQUIRE);
        }
        unsigned flags = found ? __atomic_load_n(&found->flags, __ATOMIC_ACQUIRE) : 0;
        if (!found || !(flags & VM_READ) || (write && !(flags & VM_WRITE))) {
            ok = false;
            break;
        }
        va = __atomic_load_n(&found->end, __ATOMIC_ACQUIRE);
    }
    rcu_read_unlock();
    return ok;
}

size_t vma_total_size(struct vmspace *vm)
{
    size_t total = 0;
    struct list_head *pos;
    spin_lock(&vm->lock);
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        total += v->end - v->start;
    }
    spin_unlock(&vm->lock);
    return total;
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
    if (pg)
        memset(P2V(page_to_phys(pg)), 0, PAGE_SIZE);
    spin_lock(&vm->lock);
    if (!pg)
        return false;
    struct vma *v = vma_find_locked(vm, va);
    uint64_t *entry;
    int w = paging_walk(vm->pml4_phys, va, true, &entry);
    if (!v || w != 1 || (*entry & (PTE_P | PTE_SWAPPED | PTE_PROTNONE))) {
        pmm_free_page(pg);
        return v != NULL && w == 1;
    }
    page_get(pg);
    *entry = page_to_phys(pg) | vma_pte_flags(v->flags);
    percpu_counter_inc(&vm->resident);
    return true;
}

/* Account a resolved fault to the current process. */
static void count_fault(bool major)
{
    struct thread *t = thread_current();
    if (!t)
        return;
    __atomic_fetch_add(major ? &t->proc->majflt : &t->proc->minflt, 1, __ATOMIC_RELAXED);
}

bool vma_resolve_fault(struct vmspace *vm, uintptr_t va, bool write, bool present)
{
    bool ok = false;
    spin_lock(&vm->lock);
    struct vma *v = vma_find_locked(vm, va);
    if (!v || !(v->flags & VM_READ))
        goto out;
    if (write && !(v->flags & VM_WRITE))
        goto out;
    uint64_t *entry;
    int w = paging_walk(vm->pml4_phys, va, false, &entry);
    if (present) {
        if (w == 2 && write && (*entry & PTE_COW))
            ok = huge_cow_locked(vm, va, entry) == 0;
        else if (w == 1 && (*entry & PTE_P) && write && (*entry & PTE_COW))
            ok = do_cow_locked(vm, va, entry) == 0;
        else if (w >= 1 && (*entry & PTE_P) && write && (*entry & PTE_W))
            ok = true;  /* another thread resolved it first; the stale TLB entry refaulted */
    } else if (w == 1 && (*entry & PTE_SWAPPED)) {
        spin_unlock(&vm->lock);
        ok = swap_in_page(vm, va) == 0;
        if (ok)
            count_fault(true);
        return ok;
    } else if (w == 1 && (*entry & PTE_PROTNONE)) {
        /* Cannot happen: mprotect makes the entries of a readable region
         * present again. */
        goto out;
    } else if (v->flags & VM_FILE) {
        ok = filemap_fault(vm, va, write);
        if (ok)
            count_fault(true);
        return ok;
    } else {
        if ((v->flags & VM_HUGE) && (w == 0 || (w == 1 && *entry == 0))) {
            int h = huge_fault_locked(vm, v, va);
            if (h != 0) {
                ok = h > 0;
                goto out;
            }
            /* The lock was dropped meanwhile; fall back to a small page
             * if the region is still there. */
            v = vma_find_locked(vm, va);
            if (!v || !(v->flags & VM_READ))
                goto out;
        }
        ok = fault_in_zero_page(vm, va);
    }
out:
    spin_unlock(&vm->lock);
    if (ok)
        count_fault(false);
    return ok;
}

size_t vma_count_resident(struct vmspace *vm)
{
    int64_t n = percpu_counter_sum(&vm->resident);
    return n > 0 ? (size_t)n : 0;
}

bool vmm_handle_fault(struct trapframe *tf, uintptr_t addr)
{
    struct vmspace *vm = cpu_current()->vm;
    if (!vm || vm == &kernel_vmspace || addr > USER_TOP)
        return false;
    struct fault_info fi;
    arch_fault_decode(tf, &fi);
    return vma_resolve_fault(vm, ALIGN_DOWN(addr, PAGE_SIZE), fi.write, fi.present);
}

/* Share every page of the parent with the child. Frames of private
 * regions become read only and tagged COW in both spaces, whether or not
 * they were writable, so a later mprotect never grants write access to a
 * shared frame. */
static int share_level(struct vmspace *vm, struct vmspace *child, uint64_t *src,
                       uint64_t *dst, int level, uintptr_t base)
{
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t e = src[i];
        uintptr_t va = base + ((uintptr_t)i << (12 + 9 * (level - 1)));
        if (level == 1) {
            if (!(e & (PTE_P | PTE_PROTNONE)))
                continue;
            if (!pmm_is_ram(e & PTE_ADDR_MASK)) {
                dst[i] = e;     /* device memory: shared, not counted */
                continue;
            }
            struct vma *v = vma_find_locked(vm, va);
            if (!v || (v->flags & VM_DONTFORK))
                continue;   /* the child has no region here */
            if (v->flags & VM_SHARED) {
                dst[i] = e;     /* shared object: both map the same frame */
                page_get(phys_to_page(e & PTE_ADDR_MASK));
                percpu_counter_inc(&child->resident);
                continue;
            }
            e = (e & ~PTE_W) | PTE_COW;
            src[i] = e;
            dst[i] = e;
            page_get(phys_to_page(e & PTE_ADDR_MASK));
            percpu_counter_inc(&child->resident);
        } else {
            if (!(e & PTE_P))
                continue;
            if (level == 2 && (e & PTE_PS)) {
                struct vma *v = vma_find_locked(vm, va);
                if (v && !(v->flags & VM_DONTFORK))
                    huge_share_locked(&src[i], &dst[i]);
                if (v && !(v->flags & VM_DONTFORK))
                    percpu_counter_add(&child->resident, PT_ENTRIES);
                continue;
            }
            uintptr_t table = paging_alloc_table();
            if (!table)
                return -ENOMEM;
            dst[i] = table | (e & PTE_FLAGS_MASK);
            int r = share_level(vm, child, P2V(e & PTE_ADDR_MASK), P2V(table), level - 1, va);
            if (r < 0)
                return r;
        }
    }
    return 0;
}

/* Copy the region list. Caller holds vm->lock. */
static int copy_vmas_locked(struct vmspace *vm, struct vmspace *child)
{
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->flags & VM_DONTFORK)
            continue;
        struct vma *n = kzalloc(sizeof *n);
        if (!n)
            return -ENOMEM;
        n->start = v->start;
        n->end = v->end;
        n->flags = v->flags;
        if (v->flags & VM_FILE) {
            n->file = v->file;
            file_ref(n->file);
            n->mapping = v->mapping;
            filemap_ref(n->mapping);
            n->offset = v->offset;
        }
        list_add_tail(&n->link, &child->vmas);
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
    int r = copy_vmas_locked(vm, child);
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
            r = share_level(vm, child, P2V(src[i] & PTE_ADDR_MASK), P2V(table), 3,
                            (uintptr_t)i << 39);
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
