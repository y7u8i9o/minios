#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <mm/vmm.h>
#include <sync/rcu.h>

struct file;
struct mapping;

/* A region of a user address space. Pages inside an anonymous region are
 * allocated on first touch and zero filled; pages of a file region (VM_FILE)
 * come from the file's mapping (mm/filemap.c). Protected by the owning
 * vmspace's lock. file and mapping are referenced by the region and
 * released by vma_release after the lock is dropped. */
struct vma {
    uintptr_t start;
    uintptr_t end;          /* exclusive */
    unsigned flags;         /* VM_READ, VM_WRITE, VM_EXEC and the VM_* region flags */
    struct file *file;      /* VM_FILE: the open file used for reads and writeback */
    struct mapping *mapping;/* VM_FILE: the file's page cache */
    uint64_t offset;        /* VM_FILE: file offset of start, page aligned */
    struct list_head link;  /* vmspace->vmas, sorted by start */
    struct list_head reclaim_link; /* temporary dead list; never used by RCU readers */
    struct rcu_head rcu;
};

/* Page table bits for a region's protection: present, user, writable and
 * executable as the flags say, or PTE_PROTNONE without PTE_P for a region
 * without VM_READ. */
uint64_t vma_pte_flags(unsigned flags);

/* Add [start, end) to vm. Fails with -EEXIST on overlap. */
int vma_add(struct vmspace *vm, uintptr_t start, uintptr_t end, unsigned flags);
/* Find the region containing addr. Caller holds vm->lock. */
struct vma *vma_find_locked(struct vmspace *vm, uintptr_t addr);
/* Allocate and map zero pages for every page of [start, end) that is not
 * mapped yet. Used when the kernel is about to write into a region. */
int vma_populate(struct vmspace *vm, uintptr_t start, uintptr_t end);
/* Unmap [start, end) of region v releasing frames and swap slots. Dirty
 * bits of shared file pages are recorded in the mapping. Caller holds
 * vm->lock; v may be NULL when the range holds no file pages. */
void vma_unmap_range_locked(struct vmspace *vm, struct vma *v, uintptr_t start, uintptr_t end);
/* Split v at addr (inside v, page aligned) into v and a new tail. Returns
 * the tail or NULL without memory. Caller holds vm->lock. */
struct vma *vma_split_locked(struct vmspace *vm, struct vma *v, uintptr_t addr);
/* Drop the file and mapping references of a region and free it. Called
 * with no lock held. */
void vma_release(struct vma *v);
/* Anonymous mmap: pick a free range below USER_MMAP_TOP (or use the hint
 * when it is free) and add a region. Returns the address or -errno. */
long vma_mmap(struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags);
/* The same for a file region: file and mapping are already referenced for
 * the region, fixed places the region exactly at hint. */
long vma_mmap_file(struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags, bool fixed,
                   struct file *file, struct mapping *mapping, uint64_t offset);
/* Map device memory [pa, pa + len) into a new mmap region. Returns the
 * address or -errno. */
long vma_map_device(struct vmspace *vm, uintptr_t hint, uintptr_t pa, size_t len, unsigned flags);
/* Remove mmap regions overlapping [addr, addr + len). */
int vma_munmap(struct vmspace *vm, uintptr_t addr, size_t len);
/* True if [addr, addr + len) overlaps no region that munmap could not remove. */
bool vma_range_replaceable(struct vmspace *vm, uintptr_t addr, size_t len);
/* Change the protection of [addr, addr + len) to the VM_READ/WRITE/EXEC
 * bits of prot. Splits regions as needed. */
int vma_mprotect(struct vmspace *vm, uintptr_t addr, size_t len, unsigned prot);
/* Write the dirty pages of shared file regions in [addr, addr + len) back. */
int vma_msync(struct vmspace *vm, uintptr_t addr, size_t len);
/* Writebacks of shared file ranges are queued while vm->lock is held and
 * run once it is released (mm/mmap.c). */
void vma_queue_sync(struct list_head *jobs, struct vma *v, uintptr_t start, uintptr_t end);
int vma_run_sync_jobs(struct list_head *jobs);
/* Remove every region, unmapping and releasing their frames. */
/* Remove every region before an immediate vmspace_destroy. The destroy-side
 * TLB drop replaces a redundant full-range shootdown here. */
void vma_remove_all(struct vmspace *vm);
/* Grow or shrink the heap region. Returns the old break or -errno. */
long vma_brk(struct vmspace *vm, intptr_t increment);
/* True if every page of [addr, addr + len) lies inside a region with the
 * required access. */
bool vma_range_ok(struct vmspace *vm, uintptr_t addr, size_t len, bool write);
/* Resolve a fault at the page va as the hardware reported it: write and
 * present are the error code bits. Used by the trap path and by
 * MADV_WILLNEED. Returns true when the access can be retried. */
bool vma_resolve_fault(struct vmspace *vm, uintptr_t va, bool write, bool present);
/* madvise on [addr, addr + len) (mm/madvise.c). */
long vma_madvise(struct vmspace *vm, uintptr_t addr, size_t len, int advice);
/* Number of frames present in the lower half (M40, counted on demand). */
size_t vma_count_resident(struct vmspace *vm);
/* Sum of the sizes of every region. */
size_t vma_total_size(struct vmspace *vm);

/* Duplicate vm for fork: regions are copied, present pages are shared with
 * copy on write. */
struct vmspace *vmspace_fork(struct vmspace *vm);
