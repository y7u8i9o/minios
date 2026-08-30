#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <mm/vmm.h>

/* A region of a user address space. Pages inside a region are allocated on
 * first touch and zero filled. Protected by the owning vmspace's lock. */
struct vma {
    uintptr_t start;
    uintptr_t end;          /* exclusive */
    unsigned flags;         /* VM_READ, VM_WRITE, VM_EXEC */
    struct list_head link;  /* vmspace->vmas, sorted by start */
};

/* Add [start, end) to vm. Fails with -EEXIST on overlap. */
int vma_add(struct vmspace *vm, uintptr_t start, uintptr_t end, unsigned flags);
/* Find the region containing addr. Caller holds vm->lock. */
struct vma *vma_find_locked(struct vmspace *vm, uintptr_t addr);
/* Allocate and map zero pages for every page of [start, end) that is not
 * mapped yet. Used when the kernel is about to write into a region. */
int vma_populate(struct vmspace *vm, uintptr_t start, uintptr_t end);
/* Unmap [start, end) releasing frames and swap slots. Caller holds vm->lock. */
void vma_unmap_range_locked(struct vmspace *vm, uintptr_t start, uintptr_t end);
/* Anonymous mmap: pick a free range below USER_MMAP_TOP (or use the hint
 * when it is free) and add a region. Returns the address or -errno. */
long vma_mmap(struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags);
/* Map device memory [pa, pa + len) into a new mmap region. Returns the
 * address or -errno. */
long vma_map_device(struct vmspace *vm, uintptr_t hint, uintptr_t pa, size_t len, unsigned flags);
/* Remove mmap regions overlapping [addr, addr + len). */
int vma_munmap(struct vmspace *vm, uintptr_t addr, size_t len);
/* Remove every region, unmapping and releasing their frames. */
void vma_remove_all(struct vmspace *vm);
/* Grow or shrink the heap region. Returns the old break or -errno. */
long vma_brk(struct vmspace *vm, intptr_t increment);
/* True if every page of [addr, addr + len) lies inside a region with the
 * required access. */
bool vma_range_ok(struct vmspace *vm, uintptr_t addr, size_t len, bool write);

/* Duplicate vm for fork: regions are copied, present pages are shared with
 * copy on write. */
struct vmspace *vmspace_fork(struct vmspace *vm);
