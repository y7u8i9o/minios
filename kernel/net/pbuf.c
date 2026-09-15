/* The packet buffer pool. pbuf_pool.lock protects the free list, the
 * counters and the statistics; it is a leaf taken from thread context
 * only (device completion callbacks record and wake, the worker frees). */
#define KLOG_SUBSYS "pbuf"
#include <net/pbuf.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <sync/spinlock.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

struct pbuf_pool {
    struct spinlock lock;
    struct list_head free;
    uint32_t nfree;
    uint32_t total;
    uint32_t low_water;
    uint64_t alloc_fail;
    uint64_t bad_transfer;
    struct pbuf *bufs;
    struct page *pages;
    unsigned order;
};

static struct pbuf_pool pool = {.lock = SPINLOCK_INIT("pbuf_pool")};

_Static_assert(PBUF_SIZE % PAGE_SIZE == 0 || PAGE_SIZE % PBUF_SIZE == 0, "buffers tile pages");

int pbuf_pool_init(void)
{
    size_t bytes = (size_t)NET_PBUF_COUNT * PBUF_SIZE;
    unsigned order = 0;
    while (((size_t)PAGE_SIZE << order) < bytes)
        order++;
    struct page *pages = pmm_alloc(order);
    if (!pages)
        return -ENOMEM;
    struct pbuf *bufs = kzalloc(NET_PBUF_COUNT * sizeof *bufs);
    if (!bufs) {
        pmm_free(pages, order);
        return -ENOMEM;
    }
    uint8_t *base = P2V(page_to_phys(pages));
    spin_lock(&pool.lock);
    list_init(&pool.free);
    pool.bufs = bufs;
    pool.pages = pages;
    pool.order = order;
    for (uint32_t i = 0; i < NET_PBUF_COUNT; i++) {
        struct pbuf *p = &bufs[i];
        p->buf = base + (size_t)i * PBUF_SIZE;
        p->owner = PBUF_OWNER_POOL;
        list_add_tail(&p->link, &pool.free);
    }
    pool.nfree = pool.total = pool.low_water = NET_PBUF_COUNT;
    spin_unlock(&pool.lock);
    klog_info("%u packet buffers of %u bytes, %u reserved for control",
              NET_PBUF_COUNT,
              PBUF_SIZE,
              NET_PBUF_RESERVE);
    return 0;
}

struct pbuf *pbuf_alloc(enum pbuf_class cls)
{
    spin_lock(&pool.lock);
    uint32_t floor = cls == PBUF_CONTROL ? 0 : NET_PBUF_RESERVE;
    if (pool.nfree <= floor || !pool.bufs) {
        pool.alloc_fail++;
        spin_unlock(&pool.lock);
        return NULL;
    }
    struct pbuf *p = list_first_entry(&pool.free, struct pbuf, link);
    list_del(&p->link);
    pool.nfree--;
    if (pool.nfree < pool.low_water)
        pool.low_water = pool.nfree;
    kassert(p->owner == PBUF_OWNER_POOL);
    p->owner = PBUF_OWNER_STACK;
    spin_unlock(&pool.lock);
    p->netif = NULL;
    p->data = p->buf + PBUF_HEADROOM;
    p->len = 0;
    return p;
}

void pbuf_free(struct pbuf *p)
{
    kassert(p->owner == PBUF_OWNER_STACK);
    spin_lock(&pool.lock);
    p->owner = PBUF_OWNER_POOL;
    list_add_tail(&p->link, &pool.free);
    pool.nfree++;
    spin_unlock(&pool.lock);
}

int pbuf_transfer(struct pbuf *p, enum pbuf_owner from, enum pbuf_owner to)
{
    if (from == PBUF_OWNER_POOL || to == PBUF_OWNER_POOL)
        return -EINVAL; /* the pool's side is pbuf_alloc and pbuf_free */
    if (__atomic_load_n(&p->owner, __ATOMIC_ACQUIRE) != from) {
        spin_lock(&pool.lock);
        pool.bad_transfer++;
        spin_unlock(&pool.lock);
        return -EINVAL;
    }
    __atomic_store_n(&p->owner, to, __ATOMIC_RELEASE);
    return 0;
}

size_t pbuf_headroom(const struct pbuf *p)
{
    return (size_t)(p->data - p->buf);
}

size_t pbuf_tailroom(const struct pbuf *p)
{
    return PBUF_SIZE - pbuf_headroom(p) - p->len;
}

void *pbuf_push(struct pbuf *p, size_t n)
{
    if (n > pbuf_headroom(p))
        return NULL;
    p->data -= n;
    p->len += (uint32_t)n;
    return p->data;
}

void *pbuf_pull(struct pbuf *p, size_t n)
{
    if (n > p->len)
        return NULL;
    void *old = p->data;
    p->data += n;
    p->len -= (uint32_t)n;
    return old;
}

void *pbuf_put(struct pbuf *p, size_t n)
{
    if (n > pbuf_tailroom(p))
        return NULL;
    void *tail = p->data + p->len;
    p->len += (uint32_t)n;
    return tail;
}

int pbuf_trim(struct pbuf *p, size_t len)
{
    if (len > p->len)
        return -EINVAL;
    p->len = (uint32_t)len;
    return 0;
}

void pbuf_get_stats(struct pbuf_stats *out)
{
    spin_lock(&pool.lock);
    out->total = pool.total;
    out->free = pool.nfree;
    out->reserve = NET_PBUF_RESERVE;
    out->low_water = pool.low_water;
    out->alloc_fail = pool.alloc_fail;
    out->bad_transfer = pool.bad_transfer;
    spin_unlock(&pool.lock);
}
