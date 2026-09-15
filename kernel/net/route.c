/* Static routes: one configured Ethernet interface plus the loopback /8.
 * No allocation and no table mutation outside netd. */
#include <net/ipv4.h>
#include <net/worker.h>
#include <lib/printf.h>
#include <errno.h>

static struct {
    struct netif *n;
    uint32_t address, mask, gateway;
} config;

bool ipv4_unicast(uint32_t a)
{
    return a && (a >> 24) && (a >> 24) < 224 && a != 0xffffffffu;
}

bool ipv4_local(uint32_t a)
{
    return (a >> 24) == 127 || (a && a == config.address);
}

uint32_t ipv4_address(struct netif *n)
{
    if (n == netif_loopback())
        return IPV4_LOOPBACK;
    if (n == config.n)
        return config.address;
    return 0;
}

uint32_t ipv4_netmask(struct netif *n)
{
    if (n == netif_loopback())
        return 0xff000000u;
    if (n == config.n)
        return config.mask;
    return 0;
}

uint32_t ipv4_gateway(struct netif *n)
{
    return n == config.n ? config.gateway : 0;
}

size_t ipv4_format_config(char *buf, size_t size)
{
    if (!config.n)
        return 0;
    uint32_t a = config.address, m = config.mask, g = config.gateway;
    return (size_t)ksnprintf(buf, size, "inet %s %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\n",
                             config.n->name, a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255,
                             m >> 24, (m >> 16) & 255, (m >> 8) & 255, m & 255, g >> 24,
                             (g >> 16) & 255, (g >> 8) & 255, g & 255);
}

int net_route_lookup(uint32_t dst, struct net_route *r)
{
    if (ipv4_local(dst)) {
        *r = (struct net_route){netif_loopback(), dst, dst};
    } else if (dst == IPV4_BROADCAST) {
        /* Limited broadcast leaves the attached interface even before it
         * has an address (DHCP); the source may then be 0.0.0.0. */
        if (!config.n)
            goto missing;
        *r = (struct net_route){config.n, config.address, IPV4_BROADCAST};
    } else {
        if (!ipv4_unicast(dst) || !config.n || !config.address)
            goto missing;
        uint32_t host = dst & ~config.mask;
        bool connected = (dst & config.mask) == (config.address & config.mask);
        if (connected && (!host || host == ~config.mask))
            goto missing; /* subnet and directed broadcast are not supported */
        if (!connected && !config.gateway)
            goto missing;
        *r = (struct net_route){config.n, config.address, connected ? dst : config.gateway};
    }
    return netif_is_up(r->netif) ? 0 : -ENETDOWN;
missing:
    net_ip_stats.no_route++;
    return -ENETUNREACH;
}

struct configure_request {
    struct net_request request;
    struct netif *n;
    uint32_t address, mask, gateway;
};

static int configure(struct net_request *request)
{
    struct configure_request *r = (void *)request;
    if (!r->n || !(r->n->flags & NETIF_ETHERNET))
        return -EINVAL;
    uint32_t host = ~r->mask;
    if (r->address &&
        (!ipv4_unicast(r->address) || (r->address >> 24) == 127 || !r->mask ||
         (host & (host + 1)) || host < 3 || !(r->address & host) || (r->address & host) == host))
        return -EINVAL;
    if (r->gateway && (!r->address || !ipv4_unicast(r->gateway) ||
                       (r->gateway & r->mask) != (r->address & r->mask) || !(r->gateway & host) ||
                       (r->gateway & host) == host || r->gateway == r->address))
        return -EINVAL;
    if (config.n) {
        tcp_interface_changed(config.n, ENETRESET);
        ipv4_reassembly_flush(config.n);
        arp_flush(config.n);
    }
    ipv4_path_flush();
    config.n = r->n;
    config.address = r->address;
    config.mask = r->mask;
    config.gateway = r->gateway;
    return 0;
}

int net_configure(struct netif *n, uint32_t address, uint32_t mask, uint32_t gateway)
{
    struct configure_request r = {
        .n = n,
        .address = address,
        .mask = mask,
        .gateway = gateway,
    };
    net_request_init(&r.request, configure);
    return net_worker_is_current() ? configure(&r.request) : net_request_run(&r.request);
}
