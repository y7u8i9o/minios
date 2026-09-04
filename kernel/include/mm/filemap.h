#pragma once
#include <kernel.h>
#include <sync/spinlock.h>
#include <sync/mutex.h>

struct inode;
struct file;
struct page;
struct vmspace;

/* The pages of a file that are mapped into address spaces (M37). One per
 * inode, created by the first mapping and freed with the last reference,
 * so a file that is not mapped has no cache. inode->mapping and refs are
 * protected by filemap_lock; pages, npages and the array sizes by lock (a
 * mutex, a page fill reads the file); the dirty bitmap by dirty_lock so
 * hardware dirty bits can be gathered while a vmspace lock is held. Every
 * cached frame carries one reference from the mapping plus one per page
 * table entry that maps it. */
struct mapping {
    struct inode *inode;
    int refs;                   /* regions and transient users, atomic */
    struct mutex lock;
    struct page **pages;        /* indexed by page offset in the file */
    uint64_t *dirty;            /* bit per page: written through a shared mapping, not yet on disk */
    size_t npages;              /* capacity of pages and dirty */
    struct spinlock dirty_lock;
};

/* Reference the inode's mapping, creating it when the inode has none. */
struct mapping *filemap_get(struct inode *ino);
/* Reference an existing mapping. */
void filemap_ref(struct mapping *m);
/* Drop a reference; the last one frees the cache. Dirty pages should have
 * been written back by the region that dirtied them; leftovers are logged. */
void filemap_put(struct mapping *m);
/* The mapping of ino, referenced, or NULL when the file is not mapped. */
struct mapping *filemap_lookup(struct inode *ino);

/* Record that page pgoff was written through a shared mapping. */
void filemap_mark_dirty(struct mapping *m, uint64_t pgoff);
/* Write the dirty pages in [first, last] back through file. */
int filemap_writeback(struct mapping *m, struct file *file, uint64_t first, uint64_t last);

/* Fault handler for a VM_FILE region: called with vm->lock held, returns
 * with it released. */
bool filemap_fault(struct vmspace *vm, uintptr_t va, bool write);

/* Coherence with ordinary I/O: read overlays cached pages onto data the
 * filesystem returned, write copies new data through cached pages,
 * truncate drops pages beyond the new size. Called with no mapping lock
 * held. */
void filemap_read_overlay(struct inode *ino, char *buf, uint64_t pos, size_t n);
void filemap_write_through(struct inode *ino, const char *buf, uint64_t pos, size_t n);
void filemap_truncate(struct inode *ino, uint64_t size);

struct filemap_stats {
    uint64_t cached_pages;      /* frames held by mappings right now */
    uint64_t fills;             /* pages read from files */
    uint64_t writebacks;        /* pages written back */
};
void filemap_get_stats(struct filemap_stats *out);
