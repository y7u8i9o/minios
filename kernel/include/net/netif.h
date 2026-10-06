#pragma once
/* Network interfaces (N02): a name, a link type, an MTU, a hardware
 * address, counters and an output operation. Drivers register an
 * interface after the core is initialized and mark it up once it can
 * carry frames; the loopback interface is registered by net_init. Input
 * always goes through the worker: netif_input queues the buffer, the
 * worker hands it to the interface's link input or, without one, to the
 * IP entry point. */
#include <kernel.h>
#include <lib/list.h>
#include <sync/atomic.h>
#include <net/pbuf.h>

#define NETIF_NAME_MAX 8
#define NETIF_HWADDR_LEN 6

#define NETIF_UP       (1u << 0)    /* carries traffic */
#define NETIF_LOOPBACK (1u << 1)
#define NETIF_ETHERNET (1u << 2)    /* link input and output carry Ethernet frames */

struct netif;

struct netif_ops {
    /* Transmit p (stack owned; consumed on success and on failure). */
    int (*output)(struct netif *n, struct pbuf *p);
    /* Link layer input on the worker, NULL for the IP entry point. */
    void (*input)(struct netif *n, struct pbuf *p);
};

/* Relaxed atomic counters: statistics, not synchronization. */
struct netif_stats {
    atomic_u64_t rx_packets, rx_bytes, rx_dropped;
    atomic_u64_t tx_packets, tx_bytes, tx_dropped, tx_errors;
};

/* flags are written under netif_lock and read with acquire loads on the
 * data paths; the remaining fields are set before registration and do
 * not change. link is protected by netif_lock. */
struct netif {
    char name[NETIF_NAME_MAX];
    int index;
    uint32_t flags;
    uint32_t mtu;
    uint8_t hwaddr[NETIF_HWADDR_LEN];
    const struct netif_ops *ops;
    void *priv;
    const char *driver;             /* the driver's name, for /dev/devices */
    struct netif_stats stats;
    struct list_head link;
};

void netif_init(void);
/* Publish n, down. -EEXIST for a duplicate name, -ENOSPC beyond the table. */
int netif_register(struct netif *n);
/* Take n down, let the worker finish with its queued packets, remove it. */
void netif_unregister(struct netif *n);
struct netif *netif_find(const char *name);
/* Store the first unused name of the form PREFIX0, PREFIX1, ... in name
 * (NETIF_NAME_MAX bytes). Drivers call this before netif_register. */
void netif_free_name(const char *prefix, char *name);
struct netif *netif_loopback(void);
int netif_set_up(struct netif *n, bool up);
bool netif_is_up(const struct netif *n);
/* Transmit through n: -ENETDOWN (counted, p freed) when it is down. */
int netif_output(struct netif *n, struct pbuf *p);
/* Hand a received buffer to the worker: -ENOBUFS (counted, p freed)
 * when the input queue is full. */
int netif_input(struct netif *n, struct pbuf *p);
/* Format one line per interface into buf. */
size_t netif_format_table(char *buf, size_t size);
/* One "link NAME MAC" line per Ethernet interface, for /dev/net. */
size_t netif_format_links(char *buf, size_t size);
