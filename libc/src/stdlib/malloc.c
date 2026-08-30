#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* First fit allocator over sbrk. Every block carries a header with its
 * size and a free flag; free blocks are coalesced with their successor. */
struct block {
    size_t size;            /* payload size, multiple of 16 */
    int free;
    struct block *next;
};

#define HDR ((sizeof(struct block) + 15) & ~15UL)
#define MIN_GROW (64 * 1024)

static struct block *head;

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
        n->next = b->next;
        b->size = size;
        b->next = n;
    }
}

void *malloc(size_t size)
{
    if (size == 0)
        size = 1;
    size = (size + 15) & ~15UL;
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

void free(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)((char *)p - HDR);
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

void *realloc(void *p, size_t size)
{
    if (!p)
        return malloc(size);
    if (size == 0) {
        free(p);
        return NULL;
    }
    struct block *b = (struct block *)((char *)p - HDR);
    if (b->size >= size)
        return p;
    void *n = malloc(size);
    if (!n)
        return NULL;
    memcpy(n, p, b->size);
    free(p);
    return n;
}
