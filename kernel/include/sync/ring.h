#pragma once
#include <kernel.h>
#include <lib/string.h>

/* Single-producer/single-consumer byte ring.  The producer owns head and
 * publishes bytes with a release store; the consumer owns tail and frees
 * space with a release store.  The opposite index is acquired before the
 * corresponding bytes are accessed.  Capacity must be a power of two. */
struct spsc_ring {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t tail;
};

static inline void ring_init(struct spsc_ring *r, void *data, size_t capacity)
{
    r->data = data;
    r->capacity = capacity;
    r->head = r->tail = 0;
}

static inline size_t ring_count(const struct spsc_ring *r)
{
    size_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    size_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

static inline size_t ring_space(const struct spsc_ring *r)
{
    return r->capacity - ring_count(r);
}

static inline size_t ring_write(struct spsc_ring *r, const void *src, size_t n)
{
    size_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    size_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    n = MIN(n, r->capacity - (head - tail));
    size_t off = head & (r->capacity - 1);
    size_t first = MIN(n, r->capacity - off);
    memcpy(r->data + off, src, first);
    memcpy(r->data, (const uint8_t *)src + first, n - first);
    __atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
    return n;
}

static inline size_t ring_read(struct spsc_ring *r, void *dst, size_t n)
{
    size_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    size_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    n = MIN(n, head - tail);
    size_t off = tail & (r->capacity - 1);
    size_t first = MIN(n, r->capacity - off);
    memcpy(dst, r->data + off, first);
    memcpy((uint8_t *)dst + first, r->data, n - first);
    __atomic_store_n(&r->tail, tail + n, __ATOMIC_RELEASE);
    return n;
}
