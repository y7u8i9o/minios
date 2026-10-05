#pragma once
/* Intrusive circular doubly linked list. The lock protecting a list is the
 * lock protecting the structure that embeds the list head. */
#include <kernel.h>

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void list_init(struct list_head *h)
{
    h->next = h;
    h->prev = h;
}

static inline void list_insert_between(struct list_head *n, struct list_head *prev,
                                       struct list_head *next)
{
    next->prev = n;
    n->next = next;
    n->prev = prev;
    prev->next = n;
}

static inline void list_add(struct list_head *n, struct list_head *h)
{
    list_insert_between(n, h, h->next);
}

static inline void list_add_tail(struct list_head *n, struct list_head *h)
{
    list_insert_between(n, h->prev, h);
}

static inline void list_del(struct list_head *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->next = NULL;
    n->prev = NULL;
}

static inline bool list_empty(const struct list_head *h)
{
    return h->next == h;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(h, type, member) list_entry((h)->next, type, member)
#define list_last_entry(h, type, member) list_entry((h)->prev, type, member)
#define list_for_each(pos, h) for (pos = (h)->next; pos != (h); pos = pos->next)
#define list_for_each_safe(pos, tmp, h) \
    for (pos = (h)->next, tmp = pos->next; pos != (h); pos = tmp, tmp = pos->next)
