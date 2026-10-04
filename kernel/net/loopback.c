#include <net/loopback.h>
#include <net/netif.h>
#include <net/pbuf.h>
#include <debug/panic.h>
#include <errno.h>

static int lo_output(struct netif *n, struct pbuf *p)
{
    return netif_input(n, p);
}

static const struct netif_ops lo_ops = {
    .output = lo_output,
};

static struct netif lo = {
    .name = "lo",
    .flags = NETIF_LOOPBACK,
    .mtu = LOOPBACK_MTU,
    .ops = &lo_ops,
};

struct netif *netif_loopback(void)
{
    return &lo;
}

void loopback_init(void)
{
    lo.driver = "loopback";
    if (netif_register(&lo) < 0)
        panic("net: cannot register lo");
    netif_set_up(&lo, true);
}
