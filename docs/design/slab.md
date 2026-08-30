# Kernel heap: slab allocator (M5)

## Caches

`struct kmem_cache` describes one object size. Each slab is a buddy block of
`2^slab_order` pages starting with a `struct slab` header, followed by
`objs_per_slab` slots of `stride` bytes. The order is the smallest that fits
at least four objects, capped at order 4 (64 KiB). Free slots form a singly
linked list through their first four bytes. A cache keeps a `partial` and a
`full` list; a slab whose last object is freed is returned to the buddy
allocator immediately, so an idle cache holds no memory and page accounting
in tests is exact. Each cache has its own spinlock. The list of caches is
protected by `kmem_caches_lock`. Cache descriptors are allocated from a
static bootstrap cache.

Every page of a slab is tagged `PG_SLAB` with the slab order in
`page->order`, so `kfree` finds the header by rounding an interior pointer
down to the block size and reads the owning cache from it.

## Debugging

With `CONFIG_SLABDEBUG=1` each slot is `[redzone][object][redzone]`. The
front redzone is rounded up to the cache alignment so objects keep their
alignment. On free the redzones are verified and the object is filled with
`0x6b`; on allocation the poison is verified (except the four bytes holding
the free list link) to catch writes after free. Violations panic with the
cache name and object address.

## kmalloc

Size classes are 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 and 8192 bytes.
Larger requests take a whole buddy block whose pages are tagged `PG_LARGE`
with the block order; `kfree` distinguishes the two cases by the page flags
and panics on a pointer that is neither. Results are 16 byte aligned.
`kzalloc` zeroes, `kfree(NULL)` is accepted.

Per CPU magazines can be inserted in front of `kmem_cache_alloc` in M17
without changing the interface.

## Tests

`tests/cases/slab` allocates 2000 objects of random sizes across every class
and the large path, fills and verifies patterns, frees and reallocates half,
exercises a 32 byte aligned dedicated cache, `kzalloc` and `kfree(NULL)`,
and checks after each phase that the buddy free count equals the baseline.
`tests/cases/slab_redzone` overruns an object and expects the redzone panic.
