#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include "../thread/tcb.h"

/* Segregated free lists with boundary tags over sbrk.
 *
 * Every block has a 16 byte header with its payload size and flags. A
 * free block also carries the links of its bin in the payload and a
 * footer with the size in the last 8 bytes, so that the physical
 * predecessor of a block is found in constant time when the header says
 * it is free. Free blocks are stored in bins by size: one bin per 16 bytes
 * up to 512 bytes, then one per power of two. Allocation takes the first
 * fitting block of the smallest usable bin and splits it; free coalesces
 * with both neighbours and inserts the result. Both are constant time
 * apart from a scan inside one power of two bin, which replaced the walk
 * of the whole heap that made allocation heavy programs (the Lua garbage
 * collector) quadratic in the number of live objects.
 *
 * Each region obtained from sbrk ends with a sentinel: a used header of
 * size zero, so that the forward neighbour of every block is a valid
 * header. A region that continues the previous one at the break turns
 * the old sentinel into the header of the new free block.
 *
 * Blocks of MMAP_THRESHOLD bytes or more (window buffers, images) are
 * mapped on their own and unmapped by free, so their memory goes back to
 * the kernel instead of fragmenting the heap (M33). */

struct block {
    size_t size;            /* payload bytes, a multiple of 16 */
    uint32_t free;
    uint32_t flags;
};

#define F_MAPPED    1u      /* an mmap block: not in the heap, munmap frees it */
#define F_PREV_FREE 2u      /* the physical predecessor is free and has a footer */

/* Links of a free block, stored at the start of its payload. */
struct link {
    struct block *prev, *next;
};

#define HDR ((sizeof(struct block) + 15) & ~15UL)
#define MIN_PAYLOAD 32          /* the links and the footer */
#define MIN_GROW (64 * 1024)
#define MMAP_THRESHOLD (256 * 1024)
#define PAGE 4096

#define SMALL_LIMIT 512         /* exact bins up to here, 16 bytes apart */
#define SMALL_BINS (SMALL_LIMIT / 16)
#define NBINS (SMALL_BINS + 24)

static struct block *bins[NBINS];
static char *heap_end;          /* the break: the byte after the last sentinel */
/* Protects the bins, the heap layout and the break (M35). */
static struct __libc_lock heap_lock = __LIBC_LOCK_INIT;

static inline void *payload(struct block *b) { return (char *)b + HDR; }
static inline struct block *header(void *p) { return (struct block *)((char *)p - HDR); }
static inline struct link *links(struct block *b) { return payload(b); }
static inline struct block *next_block(struct block *b) { return (struct block *)((char *)b + HDR + b->size); }
static inline size_t *footer(struct block *b) { return (size_t *)((char *)b + HDR + b->size - sizeof(size_t)); }

static int bin_index(size_t size)
{
    if (size <= SMALL_LIMIT)
        return (int)(size / 16) - 1;
    int index = SMALL_BINS;
    for (size_t limit = SMALL_LIMIT * 2; size > limit && index < NBINS - 1; limit *= 2)
        index++;
    return index;
}

static void bin_insert(struct block *b)
{
    int i = bin_index(b->size);
    struct link *l = links(b);
    l->prev = NULL;
    l->next = bins[i];
    if (bins[i])
        links(bins[i])->prev = b;
    bins[i] = b;
    b->free = 1;
    *footer(b) = b->size;
    next_block(b)->flags |= F_PREV_FREE;
}

static void bin_remove(struct block *b)
{
    struct link *l = links(b);
    if (l->prev)
        links(l->prev)->next = l->next;
    else
        bins[bin_index(b->size)] = l->next;
    if (l->next)
        links(l->next)->prev = l->prev;
    b->free = 0;
    next_block(b)->flags &= ~F_PREV_FREE;
}

/* Cuts a used block down to size when the rest can contain a free block. */
static void split(struct block *b, size_t size)
{
    if (b->size < size + HDR + MIN_PAYLOAD)
        return;
    struct block *rest = (struct block *)((char *)b + HDR + size);
    rest->size = b->size - size - HDR;
    rest->free = 0;
    rest->flags = 0;
    b->size = size;
    bin_insert(rest);
}

static void *malloc_mapped(size_t size)
{
    size_t total = (HDR + size + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    struct block *b = p;
    b->size = total - HDR;
    b->free = 0;
    b->flags = F_MAPPED;
    return payload(b);
}

/* Adds a region of at least size payload bytes as one free block. */
static struct block *grow(size_t size)
{
    size_t total = HDR + size + HDR;    /* the block and the sentinel */
    if (total < MIN_GROW)
        total = MIN_GROW;
    char *p = sbrk((intptr_t)total);
    if (p == (char *)-1)
        return NULL;
    struct block *b;
    if (heap_end && p == heap_end) {
        /* The old sentinel becomes the header of the new block. */
        b = (struct block *)(p - HDR);
        b->size = total;
    } else {
        /* A fresh region: align its first header. */
        char *aligned = (char *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
        total -= (size_t)(aligned - p);
        p = aligned;
        b = (struct block *)p;
        b->flags = 0;
        b->size = total - HDR;
    }
    b->size -= HDR;
    b->free = 0;
    struct block *sentinel = next_block(b);
    sentinel->size = 0;
    sentinel->free = 0;
    sentinel->flags = 0;
    heap_end = p + total;
    if (b->flags & F_PREV_FREE) {
        struct block *prev = (struct block *)((char *)b - HDR - *(size_t *)((char *)b - sizeof(size_t)));
        bin_remove(prev);
        prev->size += HDR + b->size;
        b = prev;
    }
    bin_insert(b);
    return b;
}

static struct block *find(size_t size)
{
    for (int i = bin_index(size); i < NBINS; i++) {
        for (struct block *b = bins[i]; b; b = links(b)->next)
            if (b->size >= size)
                return b;
    }
    return NULL;
}

static void *malloc_unlocked(size_t size)
{
    if (size < MIN_PAYLOAD)
        size = MIN_PAYLOAD;
    size = (size + 15) & ~15UL;
    if (size >= MMAP_THRESHOLD)
        return malloc_mapped(size);
    struct block *b = find(size);
    if (!b) {
        b = grow(size);
        if (!b)
            return NULL;
    }
    bin_remove(b);
    split(b, size);
    return payload(b);
}

static void free_unlocked(void *p)
{
    struct block *b = header(p);
    if (b->flags & F_MAPPED) {
        munmap(b, HDR + b->size);
        return;
    }
    struct block *n = next_block(b);
    if (n->free) {
        bin_remove(n);
        b->size += HDR + n->size;
    }
    if (b->flags & F_PREV_FREE) {
        struct block *prev = (struct block *)((char *)b - HDR - *(size_t *)((char *)b - sizeof(size_t)));
        bin_remove(prev);
        prev->size += HDR + b->size;
        b = prev;
    }
    bin_insert(b);
}

void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    void *p = malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}

static void *realloc_unlocked(void *p, size_t size)
{
    if (!p)
        return malloc_unlocked(size);
    if (size == 0) {
        free_unlocked(p);
        return NULL;
    }
    struct block *b = header(p);
    if (b->size >= size)
        return p;
    if (!(b->flags & F_MAPPED)) {
        /* Grow in place into a free successor. */
        struct block *n = next_block(b);
        size_t need = (size + 15) & ~15UL;
        if (n->free && b->size + HDR + n->size >= need) {
            bin_remove(n);
            b->size += HDR + n->size;
            split(b, need);
            return p;
        }
    }
    void *q = malloc_unlocked(size);
    if (!q)
        return NULL;
    memcpy(q, p, b->size);
    free_unlocked(p);
    return q;
}

void *malloc(size_t size)
{
    __libc_lock_lock(&heap_lock);
    void *p = malloc_unlocked(size);
    __libc_lock_unlock(&heap_lock);
    return p;
}

void free(void *p)
{
    if (!p)
        return;
    __libc_lock_lock(&heap_lock);
    free_unlocked(p);
    __libc_lock_unlock(&heap_lock);
}

void *realloc(void *p, size_t size)
{
    __libc_lock_lock(&heap_lock);
    void *q = realloc_unlocked(p, size);
    __libc_lock_unlock(&heap_lock);
    return q;
}
