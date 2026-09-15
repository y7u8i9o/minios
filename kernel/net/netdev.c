/* /dev/net: the control and diagnostic interface of the stack (N10) and
 * /dev/urandom from the kernel random provider. Reading /dev/net returns a
 * text snapshot taken on netd; the ioctls configure an interface and send
 * one ICMP echo. There is no privilege separation: MiniOS is single user. */
#include <net/ipv4.h>
#include <net/tcp.h>
#include <net/worker.h>
#include <net/pbuf.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <mm/vma.h>
#include <mm/slab.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <lib/random.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <errno.h>

struct snapshot_request {
    struct net_request request;
    char *text;
    size_t size, length;
};
static int snapshot(struct net_request *request)
{
    struct snapshot_request *r = (void *)request;
    char *b = r->text;
    size_t s = r->size, n = 0;
#define APPEND(...) n += (size_t)ksnprintf(b + n, n < s ? s - n : 0, __VA_ARGS__)
    n += netif_format_table(b, s);
    n += netif_format_links(b + n, n < s ? s - n : 0);
    n += ipv4_format_config(b + n, n < s ? s - n : 0);
    n += arp_format(b + n, n < s ? s - n : 0);
    struct ipv4_stats *i = &net_ip_stats;
    APPEND("ip invalid %lu options %lu no_route %lu too_big %lu wrong_destination %lu "
           "fragments %lu reassembled %lu frag_invalid %lu frag_full %lu frag_expired %lu "
           "pmtu_updates %lu pmtu_rejected %lu\n",
           i->invalid, i->options, i->no_route, i->too_big, i->wrong_destination, i->fragments,
           i->reassembled, i->fragment_invalid, i->fragment_full, i->fragment_expired,
           i->pmtu_updates, i->pmtu_rejected);
    APPEND("arp requests %lu timeouts %lu full %lu\n", i->arp_requests, i->arp_timeouts,
           i->arp_full);
    APPEND("icmp echo %lu reply %lu errors %lu suppressed %lu\n", i->icmp_echo,
           i->icmp_echo_reply, i->icmp_errors, i->icmp_suppressed);
    APPEND("udp invalid %lu no_port %lu full %lu\n", i->udp_invalid, i->udp_no_port, i->udp_full);
    struct tcp_stats t;
    if (tcp_get_stats(&t) == 0)
        APPEND("tcp active %lu passive %lu established %lu invalid %lu resets %lu "
               "retransmits %lu timeouts %lu backlog_drops %lu suppressed %lu connections %u "
               "half_open %u time_wait %u endpoints %u\n",
               t.active_opens, t.passive_opens, t.established, t.invalid, t.resets, t.retransmits,
               t.timeouts, t.backlog_drops, t.suppressed, t.connections, t.half_open, t.time_wait,
               t.endpoints);
    struct pbuf_stats p;
    pbuf_get_stats(&p);
    APPEND("pbuf free %u low_water %u failed %lu\n", p.free, p.low_water, p.alloc_fail);
#undef APPEND
    r->length = n;
    return 0;
}

static long netdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    enum { SIZE = 8192 };
    struct snapshot_request r = {.text = kmalloc(SIZE), .size = SIZE};
    if (!r.text)
        return -ENOMEM;
    net_request_init(&r.request, snapshot);
    long result = net_request_run(&r.request);
    if (result == 0) {
        size_t len = MIN(r.length, (size_t)SIZE - 1);
        result = 0;
        if (*pos < len) {
            n = MIN(n, len - *pos);
            memcpy(buf, r.text + *pos, n);
            *pos += n;
            result = (long)n;
        }
    }
    kfree(r.text);
    return result;
}

static long netdev_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    struct proc *p = thread_current()->proc;
    switch (req) {
    case NETIOC_CONFIGURE: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct net_config), false))
            return -EFAULT;
        struct net_config c;
        memcpy(&c, (void *)arg, sizeof c);
        c.name[sizeof c.name - 1] = 0;
        struct netif *n = netif_find(c.name);
        if (!n)
            return -ENODEV;
        return net_configure(n, c.address, c.mask, c.gateway);
    }
    case NETIOC_PING: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct net_ping), true))
            return -EFAULT;
        struct net_ping ping;
        memcpy(&ping, (void *)arg, sizeof ping);
        int result = icmp_echo(ping.address, ping.sequence, ping.size, ping.timeout_ms,
                               &ping.rtt_ms, &ping.ttl);
        memcpy((void *)arg, &ping, sizeof ping);
        return result;
    }
    }
    return -ENOTTY;
}

static long urandom_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    size_t done = 0;
    while (done < n) {
        uint32_t value;
        if (random_u32(&value) < 0)
            return done ? (long)done : -EAGAIN;
        size_t piece = MIN(n - done, sizeof value);
        memcpy(buf + done, &value, piece);
        done += piece;
    }
    return (long)done;
}

static const struct file_ops netdev_fops = {.read = netdev_read, .ioctl = netdev_ioctl};
static const struct file_ops urandom_fops = {.read = urandom_read};

void netdev_init(void)
{
    devfs_register("net", S_IFCHR | 0666, &netdev_fops, NULL, 0);
    devfs_register("urandom", S_IFCHR | 0444, &urandom_fops, NULL, 0);
}
