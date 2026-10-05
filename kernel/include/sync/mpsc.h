#pragma once
#include <kernel.h>

/* Intrusive multi-producer/single-consumer stack.  Producers publish a
 * fully initialized node with a release CAS.  The sole consumer acquires
 * the entire batch with one exchange; no producer ever removes a node, so
 * the head CAS is not exposed to ABA. */
struct mpsc_node {
    struct mpsc_node *next;
};

struct mpsc_head {
    struct mpsc_node *head;
};

#define MPSC_HEAD_INIT { .head = NULL }

static inline void mpsc_init(struct mpsc_head *q)
{
    __atomic_store_n(&q->head, NULL, __ATOMIC_RELAXED);
}

static inline void mpsc_push(struct mpsc_head *q, struct mpsc_node *node)
{
    struct mpsc_node *old = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    do {
        node->next = old;
    } while (!__atomic_compare_exchange_n(&q->head, &old, node, true,
                                           __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* mpsc_empty reports whether no node waits.  The result is a sample: a
 * producer may push a node at any time. */
static inline bool mpsc_empty(struct mpsc_head *q)
{
    return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) == NULL;
}

static inline struct mpsc_node *mpsc_take_all(struct mpsc_head *q)
{
    return __atomic_exchange_n(&q->head, NULL, __ATOMIC_ACQUIRE);
}

static inline struct mpsc_node *mpsc_reverse(struct mpsc_node *list)
{
    struct mpsc_node *out = NULL;
    while (list) {
        struct mpsc_node *next = list->next;
        list->next = out;
        out = list;
        list = next;
    }
    return out;
}
