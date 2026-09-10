#pragma once
/* Packet buffers (N02): fixed size buffers from one bounded pool made at
 * initialization, so packet memory never comes from the general
 * allocator under load. Every buffer has exactly one owner at a time:
 * the pool, the stack (protocol code or a system call), a queue, or a
 * device. A hand-over names the owner it expects and the one it installs
 * and fails when the expectation is wrong, so a buffer freed while a
 * device still reads it, or queued twice, is detected rather than
 * corrupting memory. Data allocations leave a reserve for control
 * traffic (ACKs, teardown, ARP replies), which keeps progress possible
 * when data buffers are exhausted. */
#include <kernel.h>
#include <lib/list.h>

#define PBUF_SIZE     2048          /* bytes of buffer, headroom included */
#define PBUF_HEADROOM 64            /* room for link and network headers pushed later */
#define NET_PBUF_COUNT   256        /* buffers in the pool: 512 KiB */
#define NET_PBUF_RESERVE 32         /* kept for control allocations */

enum pbuf_owner {
    PBUF_OWNER_POOL,
    PBUF_OWNER_STACK,
    PBUF_OWNER_QUEUE,
    PBUF_OWNER_DEVICE,
};

enum pbuf_class {
    PBUF_DATA,                      /* fails when only the reserve is left */
    PBUF_CONTROL,                   /* may take the reserve */
};

struct netif;

/* link is the current owner's list membership (the pool's free list
 * under pbuf_pool.lock, the worker's input queue under net_worker.lock,
 * a device's ring bookkeeping). data and len describe the valid bytes
 * inside buf; owner is written only at a checked hand-over. */
struct pbuf {
    struct list_head link;
    struct netif *netif;            /* the interface it arrived on or leaves through */
    uint8_t *data;
    uint32_t len;
    enum pbuf_owner owner;
    uint8_t *buf;                   /* PBUF_SIZE bytes */
};

struct pbuf_stats {
    uint32_t total;
    uint32_t free;
    uint32_t reserve;
    uint32_t low_water;             /* fewest free buffers seen */
    uint64_t alloc_fail;
    uint64_t bad_transfer;
};

int pbuf_pool_init(void);
/* An empty buffer owned by the stack, data at the headroom, or NULL. */
struct pbuf *pbuf_alloc(enum pbuf_class cls);
/* Return a stack owned buffer to the pool. */
void pbuf_free(struct pbuf *p);
/* Hand the buffer from one owner to another; -EINVAL when the current
 * owner is not from, counted in bad_transfer. */
int pbuf_transfer(struct pbuf *p, enum pbuf_owner from, enum pbuf_owner to);
/* Room before data and after data + len. */
size_t pbuf_headroom(const struct pbuf *p);
size_t pbuf_tailroom(const struct pbuf *p);
/* Prepend n bytes (a header), returning their start, or NULL without room. */
void *pbuf_push(struct pbuf *p, size_t n);
/* Remove n bytes from the front, returning the old start, or NULL when n > len. */
void *pbuf_pull(struct pbuf *p, size_t n);
/* Append n bytes, returning their start, or NULL without room. */
void *pbuf_put(struct pbuf *p, size_t n);
/* Shorten to len bytes; -EINVAL when len > p->len. */
int pbuf_trim(struct pbuf *p, size_t len);
void pbuf_get_stats(struct pbuf_stats *out);
