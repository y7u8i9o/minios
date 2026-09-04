#define KLOG_SUBSYS "mmap"
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/filemap.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <fs/vfs.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* True if [start, end) overlaps no region. Caller holds vm->lock. */
static bool range_free(struct vmspace *vm, uintptr_t start, uintptr_t end)
{
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->start < end && v->end > start)
            return false;
    }
    return true;
}

/* Pick the address of a new region of len bytes: the hint when it is
 * free (or required), otherwise the highest gap below USER_MMAP_TOP.
 * Caller holds vm->lock. Returns 0 when there is no room. */
static uintptr_t choose_range(struct vmspace *vm, uintptr_t hint, size_t len, bool fixed)
{
    if (fixed) {
        if (IS_ALIGNED(hint, PAGE_SIZE) && hint >= USER_BASE && hint + len - 1 <= USER_TOP &&
            hint + len > hint && range_free(vm, hint, hint + len))
            return hint;
        return 0;
    }
    if (hint && IS_ALIGNED(hint, PAGE_SIZE) && hint >= USER_BASE && hint + len <= USER_MMAP_TOP &&
        range_free(vm, hint, hint + len))
        return hint;
    /* Top down: find the highest gap below USER_MMAP_TOP. */
    uintptr_t end = USER_MMAP_TOP;
    struct list_head *pos;
    for (pos = vm->vmas.prev; pos != &vm->vmas; pos = pos->prev) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->end > end)
            continue;
        if (end - v->end >= len)
            return end - len;
        end = v->start;
    }
    if (end >= USER_BASE + len)
        return end - len;
    return 0;
}

long vma_mmap_file(struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags, bool fixed,
                   struct file *file, struct mapping *mapping, uint64_t offset)
{
    if (len == 0 || !IS_ALIGNED(len, PAGE_SIZE) || len > USER_MMAP_TOP)
        return -EINVAL;
    struct vma *n = kzalloc(sizeof *n);
    if (!n)
        return -ENOMEM;
    spin_lock(&vm->lock);
    uintptr_t start = choose_range(vm, hint, len, fixed);
    if (!start) {
        spin_unlock(&vm->lock);
        kfree(n);
        return fixed ? -EINVAL : -ENOMEM;
    }
    n->start = start;
    n->end = start + len;
    n->flags = flags | VM_MMAP;
    n->file = file;
    n->mapping = mapping;
    n->offset = offset;
    /* Keep the list sorted. */
    struct list_head *pos = vm->vmas.next;
    while (pos != &vm->vmas && list_entry(pos, struct vma, link)->start < start)
        pos = pos->next;
    list_add_tail(&n->link, pos);
    spin_unlock(&vm->lock);
    return (long)start;
}

long vma_mmap(struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags)
{
    return vma_mmap_file(vm, hint, len, flags, false, NULL, NULL, 0);
}

bool vma_range_replaceable(struct vmspace *vm, uintptr_t addr, size_t len)
{
    bool ok = true;
    spin_lock(&vm->lock);
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->start < addr + len && v->end > addr && !(v->flags & VM_MMAP)) {
            ok = false;
            break;
        }
    }
    spin_unlock(&vm->lock);
    return ok;
}

/* A writeback still to run once vm->lock is released. */
struct sync_job {
    struct mapping *mapping;
    struct file *file;
    uint64_t first, last;
    struct list_head link;
};

void vma_queue_sync(struct list_head *jobs, struct vma *v, uintptr_t start, uintptr_t end)
{
    if ((v->flags & (VM_FILE | VM_SHARED | VM_WRITE)) != (VM_FILE | VM_SHARED | VM_WRITE))
        return;
    struct sync_job *j = kmalloc(sizeof *j);
    if (!j) {
        klog_warn("no memory to queue a writeback, pages stay dirty in the cache");
        return;
    }
    j->mapping = v->mapping;
    filemap_ref(j->mapping);
    j->file = v->file;
    file_ref(j->file);
    j->first = (v->offset >> PAGE_SHIFT) + ((start - v->start) >> PAGE_SHIFT);
    j->last = j->first + ((end - start) >> PAGE_SHIFT) - 1;
    list_add_tail(&j->link, jobs);
}

int vma_run_sync_jobs(struct list_head *jobs)
{
    int r = 0;
    while (!list_empty(jobs)) {
        struct sync_job *j = list_first_entry(jobs, struct sync_job, link);
        list_del(&j->link);
        int w = filemap_writeback(j->mapping, j->file, j->first, j->last);
        if (w < 0)
            r = w;
        file_put(j->file);
        filemap_put(j->mapping);
        kfree(j);
    }
    return r;
}

int vma_munmap(struct vmspace *vm, uintptr_t addr, size_t len)
{
    if (!IS_ALIGNED(addr, PAGE_SIZE) || len == 0 || addr < USER_BASE || addr + len - 1 > USER_TOP)
        return -EINVAL;
    uintptr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    int r = 0;
    LIST_HEAD(dead);
    LIST_HEAD(jobs);
    spin_lock(&vm->lock);
    struct list_head *pos, *tmp;
    list_for_each_safe(pos, tmp, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->end <= addr || v->start >= end)
            continue;
        if (!(v->flags & VM_MMAP)) {
            r = -EINVAL;
            continue;
        }
        uintptr_t s = MAX(v->start, addr), e = MIN(v->end, end);
        if (s > v->start && e < v->end) {
            /* A hole in the middle: split first so the tail keeps its
             * file references. */
            if (!vma_split_locked(vm, v, e)) {
                r = -ENOMEM;
                break;
            }
        }
        vma_unmap_range_locked(vm, v, s, e);
        vma_queue_sync(&jobs, v, s, e);
        if (s == v->start && e == v->end) {
            list_del(&v->link);
            list_add_tail(&v->link, &dead);
        } else if (s == v->start) {
            if (v->flags & VM_FILE)
                v->offset += e - v->start;
            v->start = e;
        } else {
            v->end = s;
        }
    }
    spin_unlock(&vm->lock);
    int w = vma_run_sync_jobs(&jobs);
    if (r == 0)
        r = w;
    while (!list_empty(&dead)) {
        struct vma *v = list_first_entry(&dead, struct vma, link);
        list_del(&v->link);
        vma_release(v);
    }
    return r;
}

/* Rewrite the present entries of [start, end) for the protection in
 * flags. Copy on write frames stay read only. Caller holds vm->lock. */
static void reprotect_range_locked(struct vmspace *vm, struct vma *v, uintptr_t start, uintptr_t end,
                                   unsigned flags)
{
    uint64_t want = vma_pte_flags(flags);
    bool track_dirty = (v->flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED);
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *entry;
        if (paging_walk(vm->pml4_phys, va, false, &entry) != 1)
            continue;
        uint64_t e = *entry;
        if (!(e & (PTE_P | PTE_PROTNONE)))
            continue;
        if (track_dirty && (e & PTE_D))
            filemap_mark_dirty(v->mapping, (v->offset >> PAGE_SHIFT) + ((va - v->start) >> PAGE_SHIFT));
        uint64_t keep = e & (PTE_ADDR_MASK | PTE_COW | PTE_A | PTE_LAZYFREE);
        uint64_t bits = want;
        if (e & PTE_COW)
            bits &= ~PTE_W;
        if (!(e & PTE_COW) && (e & PTE_D) && (bits & PTE_P))
            bits |= PTE_D;
        *entry = keep | bits;
    }
    tlb_flush_range(vm, start, end - start);
}

int vma_mprotect(struct vmspace *vm, uintptr_t addr, size_t len, unsigned prot)
{
    if (!IS_ALIGNED(addr, PAGE_SIZE) || len == 0 || addr < USER_BASE || addr + len - 1 > USER_TOP ||
        addr + len < addr)
        return -EINVAL;
    uintptr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    prot &= VM_PROT_MASK;
    if (prot & (VM_WRITE | VM_EXEC))
        prot |= VM_READ;
    spin_lock(&vm->lock);
    /* The whole range must be mapped, and device regions keep their bits. */
    uintptr_t va = addr;
    while (va < end) {
        struct vma *v = vma_find_locked(vm, va);
        if (!v) {
            spin_unlock(&vm->lock);
            return -ENOMEM;
        }
        if (v->flags & VM_DEVICE) {
            spin_unlock(&vm->lock);
            return -EACCES;
        }
        if ((v->flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED) && (prot & VM_WRITE) &&
            !(v->flags & VM_WRITE) && v->file &&
            ((v->file->flags & O_ACCMODE) != O_RDWR || (v->file->flags & O_APPEND))) {
            spin_unlock(&vm->lock);
            return -EACCES;
        }
        va = v->end;
    }
    struct list_head *pos;
    list_for_each(pos, &vm->vmas) {
        struct vma *v = list_entry(pos, struct vma, link);
        if (v->end <= addr || v->start >= end)
            continue;
        if ((v->flags & VM_PROT_MASK) == prot)
            continue;
        if (v->start < addr) {
            if (!vma_split_locked(vm, v, addr)) {
                spin_unlock(&vm->lock);
                return -ENOMEM;
            }
            continue;               /* the tail is visited next */
        }
        if (v->end > end && !vma_split_locked(vm, v, end)) {
            spin_unlock(&vm->lock);
            return -ENOMEM;
        }
        v->flags = (v->flags & ~VM_PROT_MASK) | prot;
        reprotect_range_locked(vm, v, v->start, v->end, v->flags);
    }
    spin_unlock(&vm->lock);
    return 0;
}

int vma_msync(struct vmspace *vm, uintptr_t addr, size_t len)
{
    if (!IS_ALIGNED(addr, PAGE_SIZE) || addr < USER_BASE || addr + len - 1 > USER_TOP || addr + len < addr)
        return -EINVAL;
    uintptr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    LIST_HEAD(jobs);
    int r = 0;
    spin_lock(&vm->lock);
    uintptr_t va = addr;
    while (va < end) {
        struct vma *v = vma_find_locked(vm, va);
        if (!v) {
            r = -ENOMEM;
            break;
        }
        uintptr_t s = MAX(v->start, addr), e = MIN(v->end, end);
        if ((v->flags & (VM_FILE | VM_SHARED | VM_WRITE)) == (VM_FILE | VM_SHARED | VM_WRITE)) {
            /* Gather the hardware dirty bits and clear them so the next
             * write dirties the page again. */
            for (uintptr_t p = s; p < e; p += PAGE_SIZE) {
                uint64_t *entry;
                if (paging_walk(vm->pml4_phys, p, false, &entry) != 1)
                    continue;
                if ((*entry & (PTE_P | PTE_D)) == (PTE_P | PTE_D)) {
                    filemap_mark_dirty(v->mapping, (v->offset >> PAGE_SHIFT) + ((p - v->start) >> PAGE_SHIFT));
                    *entry &= ~PTE_D;
                }
            }
            tlb_flush_range(vm, s, e - s);
            vma_queue_sync(&jobs, v, s, e);
        }
        va = v->end;
    }
    spin_unlock(&vm->lock);
    int w = vma_run_sync_jobs(&jobs);
    return r ? r : w;
}

long vma_map_device(struct vmspace *vm, uintptr_t hint, uintptr_t pa, size_t len, unsigned flags)
{
    if (!IS_ALIGNED(pa, PAGE_SIZE))
        return -EINVAL;
    long va = vma_mmap(vm, hint, len, flags | VM_DEVICE);
    if (va < 0)
        return va;
    int r = vmm_map(vm, (uintptr_t)va, pa, len, flags | VM_USER);
    if (r < 0) {
        vma_munmap(vm, (uintptr_t)va, len);
        return r;
    }
    /* A driver buffer in RAM (the GPU scanout buffer) is unmapped like any
     * other frame, with a page_put per page, so take the references here;
     * the driver's own reference keeps the block alive. */
    for (size_t off = 0; off < len; off += PAGE_SIZE)
        if (pmm_is_ram(pa + off))
            page_get(phys_to_page(pa + off));
    return va;
}
