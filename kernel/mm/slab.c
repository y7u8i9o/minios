#define KLOG_SUBSYS "slab"
#include <debug/profile.h>
#include <mm/slab.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <console.h>
#include <lib/printf.h>
#include <debug/panic.h>
#include <arch/cpu.h>

/*
 * Slab layout: a struct slab header at the start of a buddy block of
 * 2^slab_order pages, followed by objs_per_slab slots of `stride` bytes.
 * Each slot is [redzone][object][redzone] with CONFIG_SLABDEBUG, the
 * object alone otherwise. Free slots form a singly linked list through
 * their first four bytes. Every page of the block is tagged PG_SLAB with
 * the slab order so kfree can find the header from any interior pointer.
 */
#define REDZONE_SIZE     16
#define REDZONE_BYTE     0x5a
#define POISON_BYTE      0x6b
#define FREE_END         0xffffffffu
#define MIN_OBJS_PER_SLAB 4
#define MAX_SLAB_ORDER   4

/* The slab header. Protected by the owning cache's lock. */
struct slab {
    struct list_head link;
    struct kmem_cache *cache;
    uint8_t *objs;              /* first slot */
    uint32_t free_count;
    uint32_t free_head;         /* slot index or FREE_END */
};

/* All caches. Protected by kmem_caches_lock. */
static LIST_HEAD(kmem_caches);
static DEFINE_SPINLOCK(kmem_caches_lock);

/* The cache descriptors themselves come from this bootstrap cache. */
static struct kmem_cache cache_cache;

#if CONFIG_SLABDEBUG
#define RZ REDZONE_SIZE
#else
#define RZ 0
#endif

static inline void *slot_obj(struct slab *s, uint32_t i)
{
    return s->objs + (size_t)i * s->cache->stride + s->cache->rz_front;
}

static inline uint32_t obj_slot(struct slab *s, const void *obj)
{
    return (uint32_t)(((const uint8_t *)obj - s->cache->rz_front - s->objs) / s->cache->stride);
}

static void cache_setup(struct kmem_cache *c, const char *name, size_t size, size_t align)
{
    if (align < 16)
        align = 16;
    c->name = name;
    c->obj_size = size;
    c->align = align;
    c->rz_front = RZ ? ALIGN_UP((size_t)RZ, align) : 0;
    c->obj_area = ALIGN_UP(size, align);
    c->stride = ALIGN_UP(c->rz_front + c->obj_area + RZ, align);
    unsigned order = 0;
    for (;;) {
        size_t avail = (PAGE_SIZE << order) - ALIGN_UP(sizeof(struct slab), align);
        if (avail / c->stride >= MIN_OBJS_PER_SLAB || order == MAX_SLAB_ORDER)
            break;
        order++;
    }
    c->slab_order = order;
    c->objs_per_slab = (unsigned)(((PAGE_SIZE << order) - ALIGN_UP(sizeof(struct slab), align)) / c->stride);
    kassert(c->objs_per_slab >= 1);
    list_init(&c->partial);
    list_init(&c->full);
    spinlock_init(&c->lock, name);
    c->nr_slabs = 0;
    c->nr_objects = 0;
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        c->magazines[i].count = 0;
        spinlock_init(&c->magazines[i].lock, "slab_magazine");
    }
}

#if CONFIG_SLABDEBUG
static void debug_init_slot(struct slab *s, uint32_t i)
{
    const struct kmem_cache *c = s->cache;
    uint8_t *obj = slot_obj(s, i);
    memset(obj - c->rz_front, REDZONE_BYTE, c->rz_front);
    memset(obj + c->obj_area, REDZONE_BYTE, c->stride - c->rz_front - c->obj_area);
    memset(obj, POISON_BYTE, c->obj_area);
}

static void debug_check_free(struct slab *s, void *obj)
{
    const struct kmem_cache *c = s->cache;
    uint8_t *p = obj;
    for (size_t i = 0; i < c->rz_front; i++) {
        if (p[i - c->rz_front] != REDZONE_BYTE)
            panic("slab %s: redzone before object %p corrupted", c->name, obj);
    }
    for (size_t i = c->obj_area; i < c->stride - c->rz_front; i++) {
        if (p[i] != REDZONE_BYTE)
            panic("slab %s: redzone after object %p corrupted", c->name, obj);
    }
    memset(p, POISON_BYTE, c->obj_area);
}

static void debug_check_alloc(struct slab *s, void *obj)
{
    uint8_t *p = obj;
    size_t size = s->cache->obj_area;
    for (size_t i = sizeof(uint32_t); i < size; i++) {
        if (p[i] != POISON_BYTE)
            panic("slab %s: object %p written after free", s->cache->name, obj);
    }
}
#else
static inline void debug_init_slot(struct slab *s, uint32_t i) { (void)s; (void)i; }
static inline void debug_check_free(struct slab *s, void *obj) { (void)s; (void)obj; }
static inline void debug_check_alloc(struct slab *s, void *obj) { (void)s; (void)obj; }
#endif

static struct slab *slab_new(struct kmem_cache *c)
{
    struct page *pg = pmm_alloc(c->slab_order);
    if (!pg)
        return NULL;
    uint64_t pfn = page_to_pfn(pg);
    for (uint64_t i = 0; i < (1UL << c->slab_order); i++) {
        struct page *p = pfn_to_page(pfn + i);
        p->flags |= PG_SLAB;
        p->order = (uint8_t)c->slab_order;
    }
    struct slab *s = P2V(page_to_phys(pg));
    s->cache = c;
    s->objs = (uint8_t *)s + ALIGN_UP(sizeof(struct slab), c->align);
    s->free_count = c->objs_per_slab;
    s->free_head = 0;
    for (uint32_t i = 0; i < c->objs_per_slab; i++) {
        debug_init_slot(s, i);
        *(uint32_t *)slot_obj(s, i) = i + 1 < c->objs_per_slab ? i + 1 : FREE_END;
    }
    c->nr_slabs++;
    return s;
}

static void slab_release(struct kmem_cache *c, struct slab *s)
{
    struct page *pg = phys_to_page(V2P(s));
    uint64_t pfn = page_to_pfn(pg);
    for (uint64_t i = 0; i < (1UL << c->slab_order); i++)
        pfn_to_page(pfn + i)->flags &= ~PG_SLAB;
    c->nr_slabs--;
    pmm_free(pg, c->slab_order);
}

static void *cache_alloc_locked(struct kmem_cache *c)
{
    struct slab *s;
    if (list_empty(&c->partial)) {
        s = slab_new(c);
        if (!s)
            return NULL;
        list_add(&s->link, &c->partial);
    } else {
        s = list_first_entry(&c->partial, struct slab, link);
    }
    uint32_t i = s->free_head;
    void *obj = slot_obj(s, i);
    s->free_head = *(uint32_t *)obj;
    s->free_count--;
    debug_check_alloc(s, obj);
    if (s->free_count == 0) {
        list_del(&s->link);
        list_add(&s->link, &c->full);
    }
    return obj;
}

static void cache_free_locked(struct kmem_cache *c, struct slab *s, void *obj)
{
    uint32_t i = obj_slot(s, obj);
    kassert(i < c->objs_per_slab && slot_obj(s, i) == obj);
    *(uint32_t *)obj = s->free_head;
    s->free_head = i;
    s->free_count++;
    if (s->free_count == 1) {
        list_del(&s->link);
        list_add(&s->link, &c->partial);
    }
    if (s->free_count == c->objs_per_slab) {
        list_del(&s->link);
        slab_release(c, s);
    }
}

static struct slab *slab_of(const void *obj);

void *kmem_cache_alloc(struct kmem_cache *c)
{
    struct kmem_magazine *m = &c->magazines[cpu_current()->id];
    spin_lock(&m->lock);
    if (m->count == 0) {
        spin_lock(&c->lock);
        while (m->count < KMEM_MAG_SIZE) {
            void *p = cache_alloc_locked(c);
            if (!p)
                break;
            m->objects[m->count++] = p;
        }
        spin_unlock(&c->lock);
    }
    void *obj = m->count ? m->objects[--m->count] : NULL;
    if (obj) {
        debug_check_alloc(slab_of(obj), obj);
        __atomic_fetch_add(&c->nr_objects, 1, __ATOMIC_RELAXED);
    }
    spin_unlock(&m->lock);
    return obj;
}

static struct slab *slab_of(const void *obj)
{
    struct page *pg = phys_to_page(V2P(obj));
    if (!(pg->flags & PG_SLAB))
        return NULL;
    uintptr_t block = ALIGN_DOWN((uintptr_t)obj, PAGE_SIZE << pg->order);
    return (struct slab *)block;
}

void kmem_cache_free(struct kmem_cache *c, void *obj)
{
    struct slab *s = slab_of(obj);
    if (!s || s->cache != c)
        panic("kmem_cache_free: %p does not belong to cache %s", obj, c->name);
    debug_check_free(s, obj);
    struct kmem_magazine *m = &c->magazines[cpu_current()->id];
    spin_lock(&m->lock);
    if (m->count == KMEM_MAG_SIZE) {
        spin_lock(&c->lock);
        for (unsigned i = 0; i < KMEM_MAG_SIZE / 2; i++) {
            void *p = m->objects[--m->count];
            cache_free_locked(c, slab_of(p), p);
        }
        spin_unlock(&c->lock);
    }
    m->objects[m->count++] = obj;
    uint64_t old = __atomic_fetch_sub(&c->nr_objects, 1, __ATOMIC_RELAXED);
    kassert(old > 0);
    spin_unlock(&m->lock);
}

static void cache_drain_magazines(struct kmem_cache *c)
{
    for (unsigned cpu = 0; cpu < MAX_CPUS; cpu++) {
        struct kmem_magazine *m = &c->magazines[cpu];
        spin_lock(&m->lock);
        spin_lock(&c->lock);
        while (m->count) {
            void *obj = m->objects[--m->count];
            cache_free_locked(c, slab_of(obj), obj);
        }
        spin_unlock(&c->lock);
        spin_unlock(&m->lock);
    }
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align)
{
    struct kmem_cache *c = kmem_cache_alloc(&cache_cache);
    if (!c)
        return NULL;
    cache_setup(c, name, size, align);
    spin_lock(&kmem_caches_lock);
    list_add_tail(&c->link, &kmem_caches);
    spin_unlock(&kmem_caches_lock);
    return c;
}

void kmem_cache_destroy(struct kmem_cache *c)
{
    cache_drain_magazines(c);
    spin_lock(&c->lock);
    if (__atomic_load_n(&c->nr_objects, __ATOMIC_RELAXED) != 0)
        panic("kmem_cache_destroy: cache %s still has %lu objects", c->name, c->nr_objects);
    spin_unlock(&c->lock);
    spin_lock(&kmem_caches_lock);
    list_del(&c->link);
    spin_unlock(&kmem_caches_lock);
    kmem_cache_free(&cache_cache, c);
}

/* kmalloc size classes. */
static const size_t kmalloc_sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
static struct kmem_cache *kmalloc_caches[ARRAY_SIZE(kmalloc_sizes)];
static char kmalloc_names[ARRAY_SIZE(kmalloc_sizes)][16];

static void *kmalloc_large(size_t size)
{
    unsigned order = 0;
    while ((PAGE_SIZE << order) < size)
        order++;
    if (order > PMM_MAX_ORDER)
        return NULL;
    struct page *pg = pmm_alloc(order);
    if (!pg)
        return NULL;
    uint64_t pfn = page_to_pfn(pg);
    for (uint64_t i = 0; i < (1UL << order); i++) {
        struct page *p = pfn_to_page(pfn + i);
        p->flags |= PG_LARGE;
        p->order = (uint8_t)order;
    }
    return P2V(page_to_phys(pg));
}

static void kfree_large(void *ptr, struct page *pg)
{
    unsigned order = pg->order;
    uintptr_t block = ALIGN_DOWN((uintptr_t)ptr, PAGE_SIZE << order);
    struct page *head = phys_to_page(V2P(block));
    uint64_t pfn = page_to_pfn(head);
    for (uint64_t i = 0; i < (1UL << order); i++)
        pfn_to_page(pfn + i)->flags &= ~PG_LARGE;
    pmm_free(head, order);
}

void *kmalloc(size_t size)
{
    if (size == 0)
        size = 1;
    void *p = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(kmalloc_sizes); i++) {
        if (size <= kmalloc_sizes[i]) {
            p = kmem_cache_alloc(kmalloc_caches[i]);
            goto done;
        }
    }
    p = kmalloc_large(size);
done:
    if (p && profile_wants(PROF_EV_ALLOC))
        profile_heap(false, p, size);
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct page *pg = phys_to_page(V2P(ptr));
    if (pg->flags & PG_SLAB) {
        struct slab *s = slab_of(ptr);
        struct kmem_cache *c = s->cache;
        if (profile_wants(PROF_EV_FREE))
            profile_heap(true, ptr, c->obj_size);
        kmem_cache_free(c, ptr);
    } else if (pg->flags & PG_LARGE) {
        if (profile_wants(PROF_EV_FREE))
            profile_heap(true, ptr, 0);
        kfree_large(ptr, pg);
    } else {
        panic("kfree: %p was not allocated by kmalloc", ptr);
    }
}

void slab_init(void)
{
    cache_setup(&cache_cache, "kmem_cache", sizeof(struct kmem_cache), 16);
    list_add_tail(&cache_cache.link, &kmem_caches);
    for (size_t i = 0; i < ARRAY_SIZE(kmalloc_sizes); i++) {
        ksnprintf(kmalloc_names[i], sizeof kmalloc_names[i], "kmalloc-%zu", kmalloc_sizes[i]);
        kmalloc_caches[i] = kmem_cache_create(kmalloc_names[i], kmalloc_sizes[i], 16);
        if (!kmalloc_caches[i])
            panic("slab: cannot create %s", kmalloc_names[i]);
    }
    klog_info("slab allocator ready, %zu kmalloc size classes, debug %d",
              ARRAY_SIZE(kmalloc_sizes), CONFIG_SLABDEBUG);
}

void slab_dump_stats(void)
{
    struct list_head *pos;
    spin_lock(&kmem_caches_lock);
    kprintf("slab caches:\n");
    list_for_each(pos, &kmem_caches) {
        struct kmem_cache *c = list_entry(pos, struct kmem_cache, link);
        spin_lock(&c->lock);
        kprintf("  %-14s obj %5zu stride %5zu order %u per slab %3u slabs %4lu objects %lu\n",
                c->name, c->obj_size, c->stride, c->slab_order, c->objs_per_slab,
                c->nr_slabs, __atomic_load_n(&c->nr_objects, __ATOMIC_RELAXED));
        spin_unlock(&c->lock);
    }
    spin_unlock(&kmem_caches_lock);
}

void slab_reclaim(void)
{
    spin_lock(&kmem_caches_lock);
    struct list_head *pos;
    list_for_each(pos, &kmem_caches) {
        struct kmem_cache *c = list_entry(pos, struct kmem_cache, link);
        cache_drain_magazines(c);
    }
    spin_unlock(&kmem_caches_lock);
}
