/* Bounded, worker-owned neighbor cache. A queued IP packet stays stack
 * owned by this module until resolution, timeout, or interface reconfiguration.
 * No endpoint pointer is retained: errors use the packet's protocol tuple. */
#include <net/ipv4.h>
#include <net/tcp.h>
#include <net/byteorder.h>
#include <net/worker.h>
#include <net/clock.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <errno.h>

#define ARP_ENTRIES 16
#define ARP_PENDING 4
#define ARP_GLOBAL 32
struct neighbor {
    struct netif *n;
    uint32_t ip;
    uint8_t mac[6];
    bool resolved;
    unsigned tries, count;
    uint64_t deadline;
    struct pbuf *pending[ARP_PENDING];
};
static struct neighbor neighbors[ARP_ENTRIES];
static unsigned pending_count;
static struct net_timer timer;
static bool initialized;
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};

static void discard(struct neighbor *a, int error)
{
    for (unsigned i = 0; i < a->count; i++) {
        udp_output_error(a->pending[i], error);
        tcp_icmp_error(a->pending[i]->data, a->pending[i]->len, error);
        pbuf_free(a->pending[i]);
        pending_count--;
    }
    memset(a, 0, sizeof *a);
}

static void emit(struct netif *n, uint16_t op, uint32_t target, const uint8_t *mac)
{
    struct pbuf *p = pbuf_alloc(PBUF_CONTROL);
    if (!p)
        return;
    uint8_t *h = pbuf_put(p, 28);
    memset(h, 0, 28);
    net_put_be16(h, 1);
    net_put_be16(h + 2, 0x0800);
    h[4] = 6;
    h[5] = 4;
    net_put_be16(h + 6, op);
    memcpy(h + 8, n->hwaddr, 6);
    net_put_be32(h + 14, ipv4_address(n));
    if (mac)
        memcpy(h + 18, mac, 6);
    net_put_be32(h + 24, target);
    ethernet_output(n, p, mac ? mac : broadcast, 0x0806);
    if (op == 1)
        net_ip_stats.arp_requests++;
}

static void schedule(void);
static void expire(struct net_timer *unused)
{
    uint64_t now = net_clock_ms();
    for (unsigned i = 0; i < ARP_ENTRIES; i++) {
        struct neighbor *a = &neighbors[i];
        if (!a->n)
            continue;
        if (!netif_is_up(a->n)) {
            discard(a, ENETDOWN);
            continue;
        }
        if (now < a->deadline)
            continue;
        if (a->resolved) {
            discard(a, EHOSTUNREACH);
            continue;
        }
        if (a->tries == 3) {
            net_ip_stats.arp_timeouts++;
            discard(a, EHOSTUNREACH);
        } else {
            emit(a->n, 1, a->ip, NULL);
            a->tries++;
            a->deadline = now + 1000;
        }
    }
    schedule();
}

static void schedule(void)
{
    uint64_t earliest = UINT64_MAX;
    if (!initialized) {
        net_timer_init(&timer, expire);
        initialized = true;
    }
    for (unsigned i = 0; i < ARP_ENTRIES; i++)
        if (neighbors[i].n && neighbors[i].deadline < earliest)
            earliest = neighbors[i].deadline;
    if (earliest == UINT64_MAX)
        net_timer_cancel(&timer);
    else
        net_timer_arm(&timer, earliest);
}

void arp_flush(struct netif *n)
{
    for (unsigned i = 0; i < ARP_ENTRIES; i++)
        if (neighbors[i].n == n)
            discard(&neighbors[i], netif_is_up(n) ? ENETRESET : ENETDOWN);
    schedule();
}

size_t arp_format(char *buf, size_t size)
{
    size_t n = 0;
    for (unsigned i = 0; i < ARP_ENTRIES; i++) {
        struct neighbor *e = &neighbors[i];
        if (!e->n)
            continue;
        uint32_t a = e->ip;
        n += (size_t)ksnprintf(buf + n, n < size ? size - n : 0,
                               "arp %u.%u.%u.%u %02x:%02x:%02x:%02x:%02x:%02x %s %s\n", a >> 24,
                               (a >> 16) & 255, (a >> 8) & 255, a & 255, e->mac[0], e->mac[1],
                               e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->n->name,
                               e->resolved ? "reachable" : "pending");
    }
    return n;
}

int arp_output(struct netif *n, struct pbuf *p, uint32_t hop)
{
    if (hop == IPV4_BROADCAST)
        return ethernet_output(n, p, broadcast, 0x0800);
    struct neighbor *a = NULL, *free_entry = NULL;
    uint64_t now = net_clock_ms();
    for (unsigned i = 0; i < ARP_ENTRIES; i++) {
        struct neighbor *e = &neighbors[i];
        if (e->n && e->resolved && e->deadline <= now)
            discard(e, EHOSTUNREACH);
        if (e->n == n && e->ip == hop)
            a = e;
        if (!e->n)
            free_entry = e;
    }
    if (a && a->resolved)
        return ipv4_link_output(n, p, a->mac);
    if (!a)
        a = free_entry;
    if (!a || a->count == ARP_PENDING || pending_count == ARP_GLOBAL) {
        net_ip_stats.arp_full++;
        pbuf_free(p);
        return -ENOBUFS;
    }
    if (!a->n) {
        a->n = n;
        a->ip = hop;
        a->tries = 1;
        a->deadline = now + 1000;
        emit(n, 1, hop, NULL);
    }
    a->pending[a->count++] = p;
    pending_count++;
    schedule();
    return 0;
}

void arp_input(struct netif *n, struct pbuf *p)
{
    uint8_t *h = p->data;
    if (p->len < 28 || net_get_be16(h) != 1 || net_get_be16(h + 2) != 0x0800 || h[4] != 6 ||
        h[5] != 4 || !ipv4_address(n))
        goto out;
    unsigned op = net_get_be16(h + 6);
    uint32_t sender = net_get_be32(h + 14), target = net_get_be32(h + 24);
    uint32_t mask = ipv4_netmask(n), host = sender & ~mask;
    static const uint8_t zero[6];
    if ((op != 1 && op != 2) || (h[8] & 1) || !memcmp(h + 8, zero, 6) ||
        target != ipv4_address(n) || sender == target || !ipv4_unicast(sender) || !host ||
        host == ~mask || (sender & mask) != (target & mask))
        goto out;
    if (op == 2 && memcmp(h + 18, n->hwaddr, 6))
        goto out;
    /* Only update an entry we already use; unsolicited responses cannot
     * consume the cache. Requests still receive a reply without caching. */
    for (unsigned i = 0; i < ARP_ENTRIES; i++) {
        struct neighbor *a = &neighbors[i];
        if (a->n != n || a->ip != sender)
            continue;
        memcpy(a->mac, h + 8, 6);
        a->resolved = true;
        a->deadline = net_clock_ms() + 60000;
        for (unsigned j = 0; j < a->count; j++) {
            ipv4_link_output(n, a->pending[j], a->mac);
            pending_count--;
        }
        a->count = 0;
    }
    if (op == 1)
        emit(n, 2, sender, h + 8);
    schedule();
out:
    pbuf_free(p);
}
