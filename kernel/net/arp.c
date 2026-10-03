/* Bounded, worker-owned neighbor cache. A queued IP packet remains stack
 * owned by this module until resolution, timeout, or interface reconfiguration.
 * No endpoint pointer is retained: errors use the packet's protocol tuple. */
#include <net/ipv4.h>
#include <net/tcp.h>
#include <net/byteorder.h>
#include <net/worker.h>
#include <net/clock.h>
#include <sched/wait.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <errno.h>
#include <kassert.h>

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

/* Address conflict probes (RFC 5227, N16), protected by arp_probe_lock. A
 * slot is reserved by a caller of arp_probe_start and released by
 * arp_probe_finish; netd marks it when another host claims the address. */
#define ARP_PROBE_SLOTS 2
struct arp_probe {
    bool used, conflict;
    struct netif *n;
    uint32_t address;
    bool announce;
    uint8_t mac[6];
    struct waitq wait;
};
static struct arp_probe probes[ARP_PROBE_SLOTS];
static DEFINE_SPINLOCK(arp_probe_lock);
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

/* A probe carries sender address 0, so no host updates its cache from
 * it; an announcement carries the address as sender and target (RFC 5227
 * sections 2.1.1 and 2.3). Both are broadcast requests with a zero target
 * hardware address. */
static void emit_probe(struct netif *n, uint32_t address, bool announce)
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
    net_put_be16(h + 6, 1);
    memcpy(h + 8, n->hwaddr, 6);
    net_put_be32(h + 14, announce ? address : 0);
    net_put_be32(h + 24, address);
    ethernet_output(n, p, broadcast, 0x0806);
    if (announce)
        net_ip_stats.arp_announcements++;
    else
        net_ip_stats.arp_probes++;
}

struct probe_request {
    struct net_request request;
    unsigned slot;
};
static int probe_send(struct net_request *request)
{
    struct arp_probe *probe = &probes[((struct probe_request *)request)->slot];
    if (!netif_is_up(probe->n))
        return -ENETDOWN;
    emit_probe(probe->n, probe->address, probe->announce);
    return 0;
}

int arp_probe_start(struct netif *n, uint32_t address, bool announce)
{
    if (!(n->flags & NETIF_ETHERNET))
        return -EOPNOTSUPP;
    if (!ipv4_unicast(address))
        return -EINVAL;
    unsigned slot;
    spin_lock(&arp_probe_lock);
    for (slot = 0; slot < ARP_PROBE_SLOTS && probes[slot].used; slot++)
        ;
    if (slot == ARP_PROBE_SLOTS) {
        spin_unlock(&arp_probe_lock);
        return -EBUSY;
    }
    struct arp_probe *probe = &probes[slot];
    memset(probe, 0, sizeof *probe);
    waitq_init(&probe->wait, "arp_probe");
    probe->used = true;
    probe->n = n;
    probe->address = address;
    probe->announce = announce;
    spin_unlock(&arp_probe_lock);
    struct probe_request r = {.slot = slot};
    net_request_init(&r.request, probe_send);
    int result = net_worker_is_current() ? probe_send(&r.request) : net_request_run(&r.request);
    if (result < 0) {
        uint8_t unused[6];
        arp_probe_finish((int)slot, 0, unused);
        return result;
    }
    return (int)slot;
}

int arp_probe_finish(int slot, unsigned wait_ms, uint8_t mac[6])
{
    kassert(slot >= 0 && slot < ARP_PROBE_SLOTS);
    kassert(!wait_ms || !net_worker_is_current());
    struct arp_probe *probe = &probes[slot];
    uint64_t deadline = timer_ms() + wait_ms;
    spin_lock(&arp_probe_lock);
    while (!probe->conflict && timer_ms() < deadline)
        waitq_wait_timeout(&probe->wait, &arp_probe_lock, deadline);
    int result = probe->conflict ? -EADDRINUSE : 0;
    memcpy(mac, probe->mac, 6);
    probe->used = false;
    spin_unlock(&arp_probe_lock);
    return result;
}

/* While an address is probed, RFC 5227 section 2.1.1 takes any ARP packet
 * whose sender is that address, and any probe for it from another hardware
 * address, as a sign that another host uses or wants the address. Packets
 * from our own hardware address are ignored. This check runs before the
 * checks that need a configured address, since probing happens before
 * configuration. */
static void probe_check(struct netif *n, const uint8_t *h, unsigned op)
{
    uint32_t sender = net_get_be32(h + 14), target = net_get_be32(h + 24);
    if ((op != 1 && op != 2) || !memcmp(h + 8, n->hwaddr, 6))
        return;
    spin_lock(&arp_probe_lock);
    for (unsigned i = 0; i < ARP_PROBE_SLOTS; i++) {
        struct arp_probe *probe = &probes[i];
        if (!probe->used || probe->n != n || probe->conflict)
            continue;
        if (sender == probe->address || (op == 1 && !sender && target == probe->address)) {
            probe->conflict = true;
            memcpy(probe->mac, h + 8, 6);
            net_ip_stats.arp_conflicts++;
            waitq_wake_all(&probe->wait);
        }
    }
    spin_unlock(&arp_probe_lock);
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
        h[5] != 4)
        goto out;
    unsigned op = net_get_be16(h + 6);
    probe_check(n, h, op);
    if (!ipv4_address(n))
        goto out;
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
