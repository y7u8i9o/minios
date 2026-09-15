#pragma once
/* The network core (N02): initialization order and the common IP input
 * entry point every interface delivers to. net_init runs from kinit,
 * after the scheduler: the pool, the interface table and the worker
 * come first; loopback, IPv4/UDP, and then the hardware NIC follow.
 * No interface can queue work before the worker runs. */
#include <kernel.h>
#include <net/pbuf.h>
#include <net/netif.h>

typedef void (*net_input_fn)(struct netif *n, struct pbuf *p);

void net_init(void);
/* The IP entry point, called on the worker with a stack owned buffer
 * that it consumes. The default handler validates and dispatches IPv4. */
void net_ip_input(struct netif *n, struct pbuf *p);
/* Replace the IP entry point (tests); NULL restores the default. */
void net_set_ip_input(net_input_fn fn);
/* Count and free a packet rejected by the default IPv4 entry point. */
void net_ip_discard(struct pbuf *p);
/* Packets rejected at the IPv4 entry point (not upper-layer statistics). */
uint64_t net_ip_input_dropped(void);
