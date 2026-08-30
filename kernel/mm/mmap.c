#define KLOG_SUBSYS "mmap"
#include <mm/vma.h>
#include <mm/slab.h>
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

long vma_mmap(struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags)
{
    if (len == 0 || !IS_ALIGNED(len, PAGE_SIZE) || len > USER_MMAP_TOP)
        return -EINVAL;
    spin_lock(&vm->lock);
    uintptr_t start = 0;
    if (hint && IS_ALIGNED(hint, PAGE_SIZE) && hint >= USER_BASE && hint + len <= USER_MMAP_TOP &&
        range_free(vm, hint, hint + len))
        start = hint;
    if (!start) {
        /* Top down: find the highest gap below USER_MMAP_TOP. */
        uintptr_t end = USER_MMAP_TOP;
        struct list_head *pos;
        for (pos = vm->vmas.prev; pos != &vm->vmas; pos = pos->prev) {
            struct vma *v = list_entry(pos, struct vma, link);
            if (v->end > end)
                continue;
            if (end - v->end >= len) {
                start = end - len;
                break;
            }
            end = v->start;
        }
        if (!start && end >= USER_BASE + len)
            start = end - len;
    }
    if (!start) {
        spin_unlock(&vm->lock);
        return -ENOMEM;
    }
    struct vma *n = kmalloc(sizeof *n);
    if (!n) {
        spin_unlock(&vm->lock);
        return -ENOMEM;
    }
    n->start = start;
    n->end = start + len;
    n->flags = flags | VM_MMAP;
    /* Keep the list sorted. */
    struct list_head *pos = vm->vmas.next;
    while (pos != &vm->vmas && list_entry(pos, struct vma, link)->start < start)
        pos = pos->next;
    list_add_tail(&n->link, pos);
    spin_unlock(&vm->lock);
    return (long)start;
}

int vma_munmap(struct vmspace *vm, uintptr_t addr, size_t len)
{
    if (!IS_ALIGNED(addr, PAGE_SIZE) || len == 0 || addr < USER_BASE || addr + len - 1 > USER_TOP)
        return -EINVAL;
    uintptr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    int r = 0;
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
        vma_unmap_range_locked(vm, s, e);
        if (s == v->start && e == v->end) {
            list_del(&v->link);
            kfree(v);
        } else if (s == v->start) {
            v->start = e;
        } else if (e == v->end) {
            v->end = s;
        } else {
            struct vma *tail = kmalloc(sizeof *tail);
            if (!tail) {
                r = -ENOMEM;
                break;
            }
            tail->start = e;
            tail->end = v->end;
            tail->flags = v->flags;
            v->end = s;
            list_add(&tail->link, &v->link);
        }
    }
    spin_unlock(&vm->lock);
    return r;
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
    return va;
}
