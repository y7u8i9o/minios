#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sync/spinlock.h>

/* A cache of equally sized objects. lock protects the slab lists and the
 * free lists inside the slabs. link is protected by kmem_caches_lock. */
struct kmem_cache {
    const char *name;
    size_t obj_size;            /* bytes requested by the creator */
    size_t align;
    size_t stride;              /* bytes per object slot including redzones */
    size_t rz_front;            /* bytes before the object, a multiple of align */
    size_t obj_area;            /* obj_size rounded up to align */
    unsigned slab_order;        /* buddy order of one slab */
    unsigned objs_per_slab;
    struct list_head partial;   /* slabs with free objects */
    struct list_head full;
    struct spinlock lock;
    struct list_head link;
    uint64_t nr_slabs;
    uint64_t nr_objects;        /* currently allocated */
};

void slab_init(void);

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align);
/* All objects must have been freed. */
void kmem_cache_destroy(struct kmem_cache *cache);
void *kmem_cache_alloc(struct kmem_cache *cache);
void kmem_cache_free(struct kmem_cache *cache, void *obj);

/* General purpose allocation. Sizes up to 8 KiB come from size class
 * caches, larger ones from whole buddy blocks. Results are 16 byte aligned.
 * kfree(NULL) is a no-op. */
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

void slab_dump_stats(void);
