/* The interface table and the two data paths. netif_lock protects the
 * list and the flags word; it is a leaf. */
#define KLOG_SUBSYS "netif"
#include <net/netif.h>
#include <net/worker.h>
#include <net/net.h>
#include <net/ipv4.h>
#include <sync/spinlock.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <klog.h>
#include <errno.h>

#define NETIF_MAX 4

static LIST_HEAD(netifs);
static DEFINE_SPINLOCK(netif_lock);
static int next_index = 1;
static int count;

void netif_init(void)
{
    /* The list and the lock are statically initialized. */
}

int netif_register(struct netif *n)
{
    if (!n->ops || !n->ops->output || !n->name[0])
        return -EINVAL;
    spin_lock(&netif_lock);
    struct list_head *pos;
    list_for_each(pos, &netifs)
    {
        struct netif *o = list_entry(pos, struct netif, link);
        if (strcmp(o->name, n->name) == 0) {
            spin_unlock(&netif_lock);
            return -EEXIST;
        }
    }
    if (count == NETIF_MAX) {
        spin_unlock(&netif_lock);
        return -ENOSPC;
    }
    n->index = next_index++;
    __atomic_store_n(&n->flags, n->flags & ~NETIF_UP, __ATOMIC_RELEASE);
    list_add_tail(&n->link, &netifs);
    count++;
    spin_unlock(&netif_lock);
    klog_info("%s registered as interface %d, mtu %u", n->name, n->index, n->mtu);
    return 0;
}

void netif_unregister(struct netif *n)
{
    netif_set_up(n, false);
    spin_lock(&netif_lock);
    list_del(&n->link);
    count--;
    spin_unlock(&netif_lock);
}

struct netif *netif_find(const char *name)
{
    struct netif *found = NULL;
    spin_lock(&netif_lock);
    struct list_head *pos;
    list_for_each(pos, &netifs)
    {
        struct netif *o = list_entry(pos, struct netif, link);
        if (strcmp(o->name, name) == 0)
            found = o;
    }
    spin_unlock(&netif_lock);
    return found;
}

bool netif_is_up(const struct netif *n)
{
    return __atomic_load_n(&n->flags, __ATOMIC_ACQUIRE) & NETIF_UP;
}

struct down_request {
    struct net_request request;
    struct netif *interface;
};
static int interface_down(struct net_request *request)
{
    struct netif *interface = ((struct down_request *)request)->interface;
    tcp_interface_changed(interface, ENETDOWN);
    ipv4_reassembly_flush(interface);
    ipv4_path_flush();
    arp_flush(interface);
    return 0;
}

/* Taking an interface down changes the flag and then waits for the
 * worker to finish its current batch, so on return no packet of the
 * interface is being processed; packets still queued are dropped when
 * the worker reaches them. */
int netif_set_up(struct netif *n, bool up)
{
    spin_lock(&netif_lock);
    uint32_t flags = n->flags;
    bool was_up = flags & NETIF_UP;
    flags = up ? flags | NETIF_UP : flags & ~NETIF_UP;
    __atomic_store_n(&n->flags, flags, __ATOMIC_RELEASE);
    spin_unlock(&netif_lock);
    if (was_up && !up) {
        struct down_request request = {.interface = n};
        net_request_init(&request.request, interface_down);
        if (net_worker_is_current())
            interface_down(&request.request);
        else {
            net_worker_drain();
            int result = net_request_run(&request.request);
            if (result < 0)
                return result;
        }
    }
    return 0;
}

int netif_output(struct netif *n, struct pbuf *p)
{
    if (!netif_is_up(n)) {
        atomic_u64_fetch_add_relaxed(&n->stats.tx_dropped, 1);
        pbuf_free(p);
        return -ENETDOWN;
    }
    uint32_t len = p->len;
    p->netif = n;
    int r = n->ops->output(n, p);
    if (r < 0) {
        atomic_u64_fetch_add_relaxed(&n->stats.tx_errors, 1);
        return r;
    }
    atomic_u64_fetch_add_relaxed(&n->stats.tx_packets, 1);
    atomic_u64_fetch_add_relaxed(&n->stats.tx_bytes, len);
    return 0;
}

int netif_input(struct netif *n, struct pbuf *p)
{
    p->netif = n;
    atomic_u64_fetch_add_relaxed(&n->stats.rx_packets, 1);
    atomic_u64_fetch_add_relaxed(&n->stats.rx_bytes, p->len);
    int r = net_worker_queue_packet(p);
    if (r < 0) {
        atomic_u64_fetch_add_relaxed(&n->stats.rx_dropped, 1);
        pbuf_free(p);
    }
    return r;
}

size_t netif_format_links(char *buf, size_t size)
{
    size_t n = 0;
    spin_lock(&netif_lock);
    struct list_head *pos;
    list_for_each(pos, &netifs)
    {
        struct netif *o = list_entry(pos, struct netif, link);
        if (!(o->flags & NETIF_ETHERNET))
            continue;
        const uint8_t *m = o->hwaddr;
        n += (size_t)ksnprintf(buf + n, n < size ? size - n : 0,
                               "link %s %02x:%02x:%02x:%02x:%02x:%02x\n", o->name, m[0], m[1],
                               m[2], m[3], m[4], m[5]);
    }
    spin_unlock(&netif_lock);
    return n;
}

size_t netif_format_table(char *buf, size_t size)
{
    size_t n = 0;
    spin_lock(&netif_lock);
    struct list_head *pos;
    list_for_each(pos, &netifs)
    {
        struct netif *o = list_entry(pos, struct netif, link);
        n += (size_t)ksnprintf(buf + n,
                               n < size ? size - n : 0,
                               "%d %s %s mtu %u rx %lu/%lu drop %lu tx %lu/%lu drop %lu err %lu\n",
                               o->index,
                               o->name,
                               netif_is_up(o) ? "up" : "down",
                               o->mtu,
                               atomic_u64_load_relaxed(&o->stats.rx_packets),
                               atomic_u64_load_relaxed(&o->stats.rx_bytes),
                               atomic_u64_load_relaxed(&o->stats.rx_dropped),
                               atomic_u64_load_relaxed(&o->stats.tx_packets),
                               atomic_u64_load_relaxed(&o->stats.tx_bytes),
                               atomic_u64_load_relaxed(&o->stats.tx_dropped),
                               atomic_u64_load_relaxed(&o->stats.tx_errors));
    }
    spin_unlock(&netif_lock);
    return n < size ? n : size - 1;
}
