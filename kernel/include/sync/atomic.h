#pragma once
#include <kernel.h>

/* Small typed wrappers around the compiler atomics.  The ordering is part
 * of each operation's name so lock-free callers cannot silently acquire a
 * stronger or weaker ordering than their proof requires. */
typedef struct {
    volatile uint32_t value;
} atomic_u32_t;

typedef struct {
    volatile uint64_t value;
} atomic_u64_t;

typedef struct {
    volatile uint32_t value;
} refcount_t;

#define ATOMIC_U32_INIT(v) { .value = (v) }
#define ATOMIC_U64_INIT(v) { .value = (v) }
#define REFCOUNT_INIT(v)   { .value = (v) }

static inline uint32_t atomic_u32_load_relaxed(const atomic_u32_t *a)
{
    return __atomic_load_n(&a->value, __ATOMIC_RELAXED);
}

static inline uint32_t atomic_u32_load_acquire(const atomic_u32_t *a)
{
    return __atomic_load_n(&a->value, __ATOMIC_ACQUIRE);
}

static inline void atomic_u32_store_relaxed(atomic_u32_t *a, uint32_t v)
{
    __atomic_store_n(&a->value, v, __ATOMIC_RELAXED);
}

static inline void atomic_u32_store_release(atomic_u32_t *a, uint32_t v)
{
    __atomic_store_n(&a->value, v, __ATOMIC_RELEASE);
}

static inline uint32_t atomic_u32_fetch_add_relaxed(atomic_u32_t *a, uint32_t v)
{
    return __atomic_fetch_add(&a->value, v, __ATOMIC_RELAXED);
}

static inline uint32_t atomic_u32_fetch_sub_relaxed(atomic_u32_t *a, uint32_t v)
{
    return __atomic_fetch_sub(&a->value, v, __ATOMIC_RELAXED);
}

static inline uint64_t atomic_u64_load_relaxed(const atomic_u64_t *a)
{
    return __atomic_load_n(&a->value, __ATOMIC_RELAXED);
}

static inline uint64_t atomic_u64_load_acquire(const atomic_u64_t *a)
{
    return __atomic_load_n(&a->value, __ATOMIC_ACQUIRE);
}

static inline void atomic_u64_store_relaxed(atomic_u64_t *a, uint64_t v)
{
    __atomic_store_n(&a->value, v, __ATOMIC_RELAXED);
}

static inline void atomic_u64_store_release(atomic_u64_t *a, uint64_t v)
{
    __atomic_store_n(&a->value, v, __ATOMIC_RELEASE);
}

static inline uint64_t atomic_u64_fetch_add_relaxed(atomic_u64_t *a, uint64_t v)
{
    return __atomic_fetch_add(&a->value, v, __ATOMIC_RELAXED);
}

static inline bool atomic_u64_cmpxchg_acq_rel(atomic_u64_t *a, uint64_t *expected,
                                               uint64_t desired)
{
    return __atomic_compare_exchange_n(&a->value, expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline void refcount_set(refcount_t *r, uint32_t value)
{
    __atomic_store_n(&r->value, value, __ATOMIC_RELAXED);
}

static inline uint32_t refcount_read(const refcount_t *r)
{
    return __atomic_load_n(&r->value, __ATOMIC_RELAXED);
}

static inline void refcount_inc(refcount_t *r)
{
    __atomic_fetch_add(&r->value, 1, __ATOMIC_RELAXED);
}

/* Acquire pairs with the release publication/removal of the pointer that
 * led the caller to this object.  A zero count is terminal. */
static inline bool refcount_inc_not_zero(refcount_t *r)
{
    uint32_t old = __atomic_load_n(&r->value, __ATOMIC_RELAXED);
    while (old != 0) {
        if (__atomic_compare_exchange_n(&r->value, &old, old + 1, true,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

/* Release publishes all object updates before the last reference goes
 * away; the acquire fence makes destruction observe them. */
static inline bool refcount_dec_and_test(refcount_t *r)
{
    if (__atomic_fetch_sub(&r->value, 1, __ATOMIC_RELEASE) != 1)
        return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return true;
}
