#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include "../thread/tcb.h"

/* First fit allocator over sbrk. Every block carries a header with its
 * size and a free flag; free blocks are coalesced with their successor.
 * Blocks of MMAP_THRESHOLD bytes or more (window buffers, images) are
 * mapped on their own and unmapped by free, so their memory goes back
 * to the kernel instead of fragmenting the heap. */
struct block {
    size_t size;            /* payload size, multiple of 16 */
    int free;
    int mapped;             /* an mmap block: not on the list, munmap frees it */
    struct block *next;
};

#define HDR ((sizeof(struct block) + 15) & ~15UL)
#define MIN_GROW (64 * 1024)
#define MMAP_THRESHOLD (256 * 1024)
#define PAGE 4096

static void *malloc_mapped(size_t size)
{
    size_t total = (HDR + size + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    struct block *b = p;
    b->size = total - HDR;
    b->free = 0;
    b->mapped = 1;
    b->next = NULL;
    return (char *)b + HDR;
}

static struct block *head;
/* Protects the block list and the break (M35). */
static struct __libc_lock heap_lock = __LIBC_LOCK_INIT;

static struct block *grow(size_t size)
{
    size_t total = HDR + size;
    if (total < MIN_GROW)
        total = MIN_GROW;
    void *p = sbrk((intptr_t)total);
    if (p == (void *)-1)
        return NULL;
    struct block *b = p;
    b->size = total - HDR;
    b->free = 1;
    b->mapped = 0;
    b->next = NULL;
    if (!head) {
        head = b;
    } else {
        struct block *last = head;
        while (last->next)
            last = last->next;
        last->next = b;
    }
    return b;
}

static void split(struct block *b, size_t size)
{
    if (b->size >= size + HDR + 16) {
        struct block *n = (struct block *)((char *)b + HDR + size);
        n->size = b->size - size - HDR;
        n->free = 1;
        n->mapped = 0;
        n->next = b->next;
        b->size = size;
        b->next = n;
    }
}

static void *malloc_unlocked(size_t size)
{
    if (size == 0)
        size = 1;
    size = (size + 15) & ~15UL;
    if (size >= MMAP_THRESHOLD)
        return malloc_mapped(size);
    for (struct block *b = head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split(b, size);
            b->free = 0;
            return (char *)b + HDR;
        }
    }
    struct block *b = grow(size);
    if (!b)
        return NULL;
    split(b, size);
    b->free = 0;
    return (char *)b + HDR;
}

static void free_unlocked(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)((char *)p - HDR);
    if (b->mapped) {
        munmap(b, HDR + b->size);
        return;
    }
    b->free = 1;
    for (struct block *c = head; c; c = c->next) {
        while (c->free && c->next && c->next->free &&
               (char *)c + HDR + c->size == (char *)c->next) {
            c->size += HDR + c->next->size;
            c->next = c->next->next;
        }
    }
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
    struct block *b = (struct block *)((char *)p - HDR);
    if (b->size >= size)
        return p;
    void *n = malloc_unlocked(size);
    if (!n)
        return NULL;
    memcpy(n, p, b->size);
    free_unlocked(p);
    return n;
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
