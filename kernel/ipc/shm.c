#define KLOG_SUBSYS "shm"
#include <ipc/mqueue.h>
#include <fs/vfs.h>
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

#define SHM_MAX 32
#define SHM_MAX_PAGES 4096

/* A named shared memory object: frames that several address spaces map.
 * The object holds one reference on every frame; mappings add their own
 * so unmapping through vma_unmap_range_locked is uniform. The table and
 * refs are protected by shm_lock. */
struct shm {
    char name[SHM_NAME_MAX];
    int refs;
    bool unlinked;
    bool anon;                          /* memfd: no name, grows by truncation */
    size_t npages;
    struct page **pages;
};

static struct shm *objects[SHM_MAX];
static DEFINE_SPINLOCK(shm_lock);

static void shm_free(struct shm *s)
{
    for (size_t i = 0; i < s->npages; i++)
        if (s->pages[i])
            page_put(s->pages[i]);
    kfree(s->pages);
    kfree(s);
}

static void shm_release(struct file *f)
{
    struct shm *s = f->priv;
    spin_lock(&shm_lock);
    bool free_it = --s->refs == 0 && s->unlinked;
    if (free_it)
        for (int i = 0; i < SHM_MAX; i++)
            if (objects[i] == s)
                objects[i] = NULL;
    spin_unlock(&shm_lock);
    if (free_it)
        shm_free(s);
}

static long shm_mmap(struct file *f, struct vmspace *vm, uintptr_t hint, size_t len, unsigned flags,
                     uint64_t off)
{
    struct shm *s = f->priv;
    spin_lock(&shm_lock);
    size_t npages = s->npages;
    spin_unlock(&shm_lock);
    if (off || len > npages * PAGE_SIZE)
        return -EINVAL;
    long va = vma_mmap(vm, hint, len, flags | VM_SHARED);
    if (va < 0)
        return va;
    for (size_t i = 0; i < len / PAGE_SIZE; i++) {
        page_get(s->pages[i]);
        int r = vmm_map(vm, (uintptr_t)va + i * PAGE_SIZE, page_to_phys(s->pages[i]), PAGE_SIZE,
                        flags | VM_USER);
        if (r < 0) {
            page_put(s->pages[i]);
            vma_munmap(vm, (uintptr_t)va, len);
            return r;
        }
    }
    return va;
}

/* Grow an anonymous object (memfd) to size; shrinking is not supported. */
static int shm_truncate(struct file *f, uint64_t size)
{
    struct shm *s = f->priv;
    size_t npages = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    if (!s->anon || npages > SHM_MAX_PAGES)
        return -EINVAL;
    if (npages <= s->npages)
        return npages == s->npages ? 0 : -EINVAL;
    struct page **pages = kzalloc(npages * sizeof *pages);
    if (!pages)
        return -ENOMEM;
    for (size_t i = s->npages; i < npages; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) {
            for (size_t j = s->npages; j < i; j++)
                page_put(pages[j]);
            kfree(pages);
            return -ENOMEM;
        }
        page_get(pages[i]);
        memset(P2V(page_to_phys(pages[i])), 0, PAGE_SIZE);
    }
    spin_lock(&shm_lock);
    memcpy(pages, s->pages, s->npages * sizeof *pages);
    struct page **old = s->pages;
    s->pages = pages;
    s->npages = npages;
    spin_unlock(&shm_lock);
    kfree(old);
    return 0;
}

static const struct file_ops shm_fops = {
    .mmap = shm_mmap,
    .release = shm_release,
    .truncate = shm_truncate,
};

int shm_create_anon(struct file **out)
{
    struct shm *s = kzalloc(sizeof *s);
    if (!s)
        return -ENOMEM;
    s->anon = true;
    s->unlinked = true;                 /* freed with its last descriptor */
    s->refs = 1;
    s->pages = kzalloc(sizeof *s->pages);
    struct file *f = file_alloc(NULL, &shm_fops, O_RDWR);
    if (!s->pages || !f) {
        kfree(s->pages);
        kfree(s);
        return -ENOMEM;
    }
    f->priv = s;
    *out = f;
    return 0;
}

int shm_open(const char *name, int flags, size_t size, struct file **out)
{
    if (!name[0] || strlen(name) >= SHM_NAME_MAX)
        return -EINVAL;
    size = ALIGN_UP(size, PAGE_SIZE);
    spin_lock(&shm_lock);
    struct shm *s = NULL;
    int slot = -1;
    for (int i = 0; i < SHM_MAX; i++) {
        if (objects[i] && !objects[i]->unlinked && strcmp(objects[i]->name, name) == 0)
            s = objects[i];
        else if (!objects[i] && slot < 0)
            slot = i;
    }
    if (s && (flags & SHM_EXCL)) {
        spin_unlock(&shm_lock);
        return -EEXIST;
    }
    if (!s) {
        if (!(flags & SHM_CREATE) || size == 0 || size / PAGE_SIZE > SHM_MAX_PAGES || slot < 0) {
            spin_unlock(&shm_lock);
            return !(flags & SHM_CREATE) ? -ENOENT : slot < 0 ? -ENOSPC : -EINVAL;
        }
        spin_unlock(&shm_lock);
        s = kzalloc(sizeof *s);
        if (!s)
            return -ENOMEM;
        strlcpy(s->name, name, sizeof s->name);
        s->npages = size / PAGE_SIZE;
        s->pages = kzalloc(s->npages * sizeof *s->pages);
        if (!s->pages) {
            kfree(s);
            return -ENOMEM;
        }
        for (size_t i = 0; i < s->npages; i++) {
            s->pages[i] = pmm_alloc_page();
            if (!s->pages[i]) {
                shm_free(s);
                return -ENOMEM;
            }
            page_get(s->pages[i]);
            memset(P2V(page_to_phys(s->pages[i])), 0, PAGE_SIZE);
        }
        spin_lock(&shm_lock);
        struct shm *other = NULL;
        for (int i = 0; i < SHM_MAX; i++)
            if (objects[i] && !objects[i]->unlinked && strcmp(objects[i]->name, name) == 0)
                other = objects[i];
        if (other) {
            spin_unlock(&shm_lock);
            shm_free(s);
            spin_lock(&shm_lock);
            s = other;
        } else if (!objects[slot]) {
            objects[slot] = s;
        } else {
            spin_unlock(&shm_lock);
            shm_free(s);
            return -ENOSPC;
        }
    }
    s->refs++;
    spin_unlock(&shm_lock);
    struct file *f = file_alloc(NULL, &shm_fops, O_RDWR);
    if (!f) {
        shm_release(&(struct file){ .priv = s });
        return -ENOMEM;
    }
    f->priv = s;
    *out = f;
    return 0;
}

int shm_unlink(const char *name)
{
    spin_lock(&shm_lock);
    for (int i = 0; i < SHM_MAX; i++) {
        struct shm *s = objects[i];
        if (s && !s->unlinked && strcmp(s->name, name) == 0) {
            s->unlinked = true;
            bool free_it = s->refs == 0;
            if (free_it)
                objects[i] = NULL;
            spin_unlock(&shm_lock);
            if (free_it)
                shm_free(s);
            return 0;
        }
    }
    spin_unlock(&shm_lock);
    return -ENOENT;
}
