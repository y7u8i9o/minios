#define KLOG_SUBSYS "filemap"
#include <mm/filemap.h>
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/swap.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* inode->mapping pointers and mapping reference counts. */
static DEFINE_SPINLOCK(filemap_lock);
/* Statistics, atomic updates. */
static struct filemap_stats stats;

static struct mapping *mapping_alloc(struct inode *ino)
{
    struct mapping *m = kzalloc(sizeof *m);
    if (!m)
        return NULL;
    m->inode = ino;
    m->refs = 1;
    mutex_init(&m->lock, "mapping");
    spinlock_init(&m->dirty_lock, "mapping_dirty");
    return m;
}

static void mapping_free(struct mapping *m)
{
    for (size_t i = 0; i < m->npages; i++) {
        if (!m->pages[i])
            continue;
        if (m->dirty[i / 64] & (1UL << (i % 64)))
            klog_warn("inode %lu: dirty page %zu dropped without writeback", m->inode->ino, i);
        page_put(m->pages[i]);
        __atomic_fetch_sub(&stats.cached_pages, 1, __ATOMIC_RELAXED);
    }
    kfree(m->pages);
    kfree(m->dirty);
    kfree(m);
}

struct mapping *filemap_get(struct inode *ino)
{
    spin_lock(&filemap_lock);
    struct mapping *m = ino->mapping;
    if (m) {
        __atomic_fetch_add(&m->refs, 1, __ATOMIC_SEQ_CST);
        spin_unlock(&filemap_lock);
        return m;
    }
    spin_unlock(&filemap_lock);
    struct mapping *n = mapping_alloc(ino);
    if (!n)
        return NULL;
    spin_lock(&filemap_lock);
    m = ino->mapping;
    if (m) {
        __atomic_fetch_add(&m->refs, 1, __ATOMIC_SEQ_CST);
        spin_unlock(&filemap_lock);
        kfree(n);
        return m;
    }
    ino->mapping = n;
    spin_unlock(&filemap_lock);
    return n;
}

void filemap_ref(struct mapping *m)
{
    __atomic_fetch_add(&m->refs, 1, __ATOMIC_SEQ_CST);
}

void filemap_put(struct mapping *m)
{
    if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_SEQ_CST) != 0)
        return;
    /* Nobody references the mapping; detach it unless a lookup revived it
     * meanwhile (a lookup takes filemap_lock and increments refs). */
    spin_lock(&filemap_lock);
    if (__atomic_load_n(&m->refs, __ATOMIC_SEQ_CST) != 0) {
        spin_unlock(&filemap_lock);
        return;
    }
    if (m->inode->mapping == m)
        m->inode->mapping = NULL;
    spin_unlock(&filemap_lock);
    mapping_free(m);
}

struct mapping *filemap_lookup(struct inode *ino)
{
    spin_lock(&filemap_lock);
    struct mapping *m = ino->mapping;
    if (m)
        __atomic_fetch_add(&m->refs, 1, __ATOMIC_SEQ_CST);
    spin_unlock(&filemap_lock);
    return m;
}

void filemap_mark_dirty(struct mapping *m, uint64_t pgoff)
{
    spin_lock(&m->dirty_lock);
    if (pgoff < m->npages)
        m->dirty[pgoff / 64] |= 1UL << (pgoff % 64);
    spin_unlock(&m->dirty_lock);
}

/* Test and clear the dirty bit of pgoff. */
static bool take_dirty(struct mapping *m, uint64_t pgoff)
{
    spin_lock(&m->dirty_lock);
    bool d = pgoff < m->npages && (m->dirty[pgoff / 64] & (1UL << (pgoff % 64)));
    if (d)
        m->dirty[pgoff / 64] &= ~(1UL << (pgoff % 64));
    spin_unlock(&m->dirty_lock);
    return d;
}

/* Make room for page index pgoff. Caller holds m->lock. */
static int grow(struct mapping *m, uint64_t pgoff)
{
    if (pgoff < m->npages)
        return 0;
    size_t n = m->npages ? m->npages : 16;
    while (n <= pgoff)
        n *= 2;
    struct page **pages = kzalloc(n * sizeof *pages);
    uint64_t *dirty = kzalloc(ALIGN_UP(n, 64) / 8);
    if (!pages || !dirty) {
        kfree(pages);
        kfree(dirty);
        return -ENOMEM;
    }
    if (m->npages) {
        memcpy(pages, m->pages, m->npages * sizeof *pages);
        spin_lock(&m->dirty_lock);
        memcpy(dirty, m->dirty, ALIGN_UP(m->npages, 64) / 8);
        uint64_t *old_dirty = m->dirty;
        m->dirty = dirty;
        m->npages = n;
        spin_unlock(&m->dirty_lock);
        kfree(old_dirty);
        kfree(m->pages);
    } else {
        spin_lock(&m->dirty_lock);
        m->dirty = dirty;
        m->npages = n;
        spin_unlock(&m->dirty_lock);
    }
    m->pages = pages;
    return 0;
}

/* Read page pgoff of the file into a fresh frame. Caller holds m->lock. */
static struct page *fill_page(struct mapping *m, struct file *file, uint64_t pgoff)
{
    struct page *pg = swap_alloc_user_frame();
    if (!pg)
        return NULL;
    char *buf = P2V(page_to_phys(pg));
    uint64_t pos = pgoff << PAGE_SHIFT;
    size_t got = 0;
    while (got < PAGE_SIZE) {
        long r = file->ops->read(file, buf + got, PAGE_SIZE - got, &pos);
        if (r < 0) {
            pmm_free_page(pg);
            return NULL;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    if (got < PAGE_SIZE)
        memset(buf + got, 0, PAGE_SIZE - got);
    __atomic_fetch_add(&stats.fills, 1, __ATOMIC_RELAXED);
    return pg;
}

/* The cached page pgoff, read through file when absent. Returns the page
 * with a reference for the caller, or NULL beyond the end of the file or
 * without memory. */
static struct page *mapping_get_page(struct mapping *m, struct file *file, uint64_t pgoff)
{
    mutex_lock(&m->lock);
    uint64_t size = m->inode->size;
    if (pgoff >= ALIGN_UP(size, PAGE_SIZE) >> PAGE_SHIFT) {
        mutex_unlock(&m->lock);
        return NULL;
    }
    if (pgoff < m->npages && m->pages[pgoff]) {
        struct page *pg = m->pages[pgoff];
        page_get(pg);
        mutex_unlock(&m->lock);
        return pg;
    }
    if (grow(m, pgoff) < 0) {
        mutex_unlock(&m->lock);
        return NULL;
    }
    struct page *pg = fill_page(m, file, pgoff);
    if (pg) {
        page_get(pg);               /* the mapping's reference */
        page_get(pg);               /* the caller's */
        m->pages[pgoff] = pg;
        __atomic_fetch_add(&stats.cached_pages, 1, __ATOMIC_RELAXED);
    }
    mutex_unlock(&m->lock);
    return pg;
}

int filemap_writeback(struct mapping *m, struct file *file, uint64_t first, uint64_t last)
{
    int r = 0;
    vfs_op_begin(m->inode->sb);
    mutex_lock(&m->lock);
    if (last >= m->npages)
        last = m->npages ? m->npages - 1 : 0;
    for (uint64_t i = first; i <= last && i < m->npages; i++) {
        if (!take_dirty(m, i) || !m->pages[i])
            continue;
        uint64_t pos = i << PAGE_SHIFT;
        uint64_t size = m->inode->size;
        if (pos >= size)
            continue;
        size_t n = MIN(PAGE_SIZE, size - pos);
        long w = file->ops->write(file, P2V(page_to_phys(m->pages[i])), n, &pos);
        if (w < 0) {
            klog_warn("inode %lu: writeback of page %lu failed: %ld", m->inode->ino, i, w);
            filemap_mark_dirty(m, i);
            r = (int)w;
        } else {
            __atomic_fetch_add(&stats.writebacks, 1, __ATOMIC_RELAXED);
        }
    }
    mutex_unlock(&m->lock);
    vfs_op_end(m->inode->sb);
    return r;
}

bool filemap_fault(struct vmspace *vm, uintptr_t va, bool write)
{
    struct vma *v = vma_find_locked(vm, va);
    if (!v || !(v->flags & VM_FILE) || !(v->flags & VM_READ)) {
        spin_unlock(&vm->lock);
        return false;
    }
    struct mapping *m = v->mapping;
    struct file *file = v->file;
    unsigned flags = v->flags;
    uint64_t pgoff = (v->offset >> PAGE_SHIFT) + ((va - v->start) >> PAGE_SHIFT);
    filemap_ref(m);
    file_ref(file);
    spin_unlock(&vm->lock);

    bool ok = false;
    struct page *pg = mapping_get_page(m, file, pgoff);
    if (!pg)
        goto out;
    pte_t pte;
    if (flags & VM_SHARED) {
        pte = vma_make_pte(page_to_phys(pg), flags);
    } else if (write) {
        /* A private write: the region gets its own copy of the page. */
        struct page *copy = swap_alloc_user_frame();
        if (!copy) {
            page_put(pg);
            goto out;
        }
        memcpy(P2V(page_to_phys(copy)), P2V(page_to_phys(pg)), PAGE_SIZE);
        page_put(pg);
        page_get(copy);
        pg = copy;
        pte = vma_make_pte(page_to_phys(pg), flags);
    } else {
        /* A private read maps the cached frame read only; the copy on write
         * bit makes a later write copy it. */
        pte = pte_mkcow(pte_wrprotect(vma_make_pte(page_to_phys(pg), flags)));
    }

    spin_lock(&vm->lock);
    struct vma *cur = vma_find_locked(vm, va);
    pte_t *entry;
    int w = paging_walk(vm->pt_root, va, true, &entry);
    if (cur != v || !(cur->flags & VM_FILE) || w != 1) {
        page_put(pg);
        ok = cur != NULL && w == 1;
    } else if (pte_mapped(*entry) || pte_swapped(*entry)) {
        page_put(pg);           /* another thread mapped it meanwhile */
        ok = true;
    } else {
        *entry = pte;
        percpu_counter_inc(&vm->resident);
        ok = true;
    }
    spin_unlock(&vm->lock);
out:
    file_put(file);
    filemap_put(m);
    return ok;
}

void filemap_read_overlay(struct inode *ino, char *buf, uint64_t pos, size_t n)
{
    struct mapping *m = filemap_lookup(ino);
    if (!m)
        return;
    mutex_lock(&m->lock);
    uint64_t size = ino->size;
    uint64_t end = MIN(pos + n, size);
    for (uint64_t p = pos; p < end;) {
        uint64_t pgoff = p >> PAGE_SHIFT;
        size_t in_page = PAGE_SIZE - (p & (PAGE_SIZE - 1));
        size_t chunk = MIN(in_page, end - p);
        if (pgoff < m->npages && m->pages[pgoff])
            memcpy(buf + (p - pos), (char *)P2V(page_to_phys(m->pages[pgoff])) + (p & (PAGE_SIZE - 1)), chunk);
        p += chunk;
    }
    mutex_unlock(&m->lock);
    filemap_put(m);
}

void filemap_write_through(struct inode *ino, const char *buf, uint64_t pos, size_t n)
{
    struct mapping *m = filemap_lookup(ino);
    if (!m)
        return;
    mutex_lock(&m->lock);
    for (uint64_t p = pos; p < pos + n;) {
        uint64_t pgoff = p >> PAGE_SHIFT;
        size_t in_page = PAGE_SIZE - (p & (PAGE_SIZE - 1));
        size_t chunk = MIN(in_page, pos + n - p);
        if (pgoff < m->npages && m->pages[pgoff])
            memcpy((char *)P2V(page_to_phys(m->pages[pgoff])) + (p & (PAGE_SIZE - 1)), buf + (p - pos), chunk);
        p += chunk;
    }
    mutex_unlock(&m->lock);
    filemap_put(m);
}

void filemap_truncate(struct inode *ino, uint64_t size)
{
    struct mapping *m = filemap_lookup(ino);
    if (!m)
        return;
    mutex_lock(&m->lock);
    uint64_t keep = ALIGN_UP(size, PAGE_SIZE) >> PAGE_SHIFT;
    for (uint64_t i = keep; i < m->npages; i++) {
        if (!m->pages[i])
            continue;
        take_dirty(m, i);
        page_put(m->pages[i]);
        m->pages[i] = NULL;
        __atomic_fetch_sub(&stats.cached_pages, 1, __ATOMIC_RELAXED);
    }
    if (keep && keep - 1 < m->npages && m->pages[keep - 1] && (size & (PAGE_SIZE - 1)))
        memset((char *)P2V(page_to_phys(m->pages[keep - 1])) + (size & (PAGE_SIZE - 1)), 0,
               PAGE_SIZE - (size & (PAGE_SIZE - 1)));
    mutex_unlock(&m->lock);
    filemap_put(m);
}

void filemap_get_stats(struct filemap_stats *out)
{
    out->cached_pages = __atomic_load_n(&stats.cached_pages, __ATOMIC_RELAXED);
    out->fills = __atomic_load_n(&stats.fills, __ATOMIC_RELAXED);
    out->writebacks = __atomic_load_n(&stats.writebacks, __ATOMIC_RELAXED);
}
