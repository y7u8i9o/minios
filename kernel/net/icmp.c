#include <net/ipv4.h>
#include <net/tcp.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <net/clock.h>
#include <net/worker.h>
#include <sched/wait.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <errno.h>

/* Echo requests from /dev/net: slots protected by icmp_lock (leaf). The
 * worker emits and matches; the caller sleeps with a bounded deadline. */
#define ECHO_SLOTS 4
struct echo {
    bool used, done;
    uint16_t sequence;
    uint32_t destination;
    int result;
    uint8_t ttl;
    uint64_t sent_ms, rtt_ms;
    size_t size;
    struct waitq wait;
};
static struct echo echoes[ECHO_SLOTS];
static DEFINE_SPINLOCK(icmp_lock);

static void echo_complete(unsigned slot, uint16_t sequence, int result, uint8_t ttl)
{
    spin_lock(&icmp_lock);
    struct echo *e = &echoes[slot];
    if (e->used && !e->done && e->sequence == sequence) {
        e->done = true;
        e->result = result;
        e->ttl = ttl;
        e->rtt_ms = net_clock_ms() - e->sent_ms;
        waitq_wake_all(&e->wait);
    }
    spin_unlock(&icmp_lock);
}

struct echo_request {
    struct net_request request;
    unsigned slot;
};
static int echo_send(struct net_request *request)
{
    struct echo *e = &echoes[((struct echo_request *)request)->slot];
    struct pbuf *p = pbuf_alloc(PBUF_DATA);
    if (!p)
        return -ENOBUFS;
    uint8_t *h = pbuf_put(p, 8 + e->size);
    memset(h, 0, 8 + e->size);
    h[0] = 8;
    net_put_be16(h + 4, 0x4d00 + ((struct echo_request *)request)->slot);
    net_put_be16(h + 6, e->sequence);
    for (size_t i = 0; i < e->size; i++)
        h[8 + i] = (uint8_t)i;
    net_put_be16(h + 2, net_checksum(h, 8 + e->size));
    e->sent_ms = net_clock_ms();
    return ipv4_output(p, 0, e->destination, 1);
}

int icmp_echo(uint32_t destination, uint16_t sequence, size_t size, unsigned timeout_ms,
              uint32_t *rtt_ms, uint8_t *ttl)
{
    if (size > 1400 || timeout_ms > 60000 || !ipv4_unicast(destination))
        return -EINVAL;
    unsigned slot;
    spin_lock(&icmp_lock);
    for (slot = 0; slot < ECHO_SLOTS && echoes[slot].used; slot++)
        ;
    if (slot == ECHO_SLOTS) {
        spin_unlock(&icmp_lock);
        return -EBUSY;
    }
    struct echo *e = &echoes[slot];
    memset(e, 0, sizeof *e);
    waitq_init(&e->wait, "icmp_echo");
    e->used = true;
    e->sequence = sequence;
    e->destination = destination;
    e->size = size;
    spin_unlock(&icmp_lock);
    struct echo_request r = {.slot = slot};
    net_request_init(&r.request, echo_send);
    int result = net_request_run(&r.request);
    uint64_t deadline = timer_ms() + timeout_ms;
    spin_lock(&icmp_lock);
    if (result < 0) {
        e->done = true;
        e->result = result;
    }
    while (!e->done && timer_ms() < deadline)
        waitq_wait_timeout(&e->wait, &icmp_lock, deadline);
    result = e->done ? e->result : -ETIMEDOUT;
    *rtt_ms = (uint32_t)e->rtt_ms;
    *ttl = e->ttl;
    e->used = false;
    spin_unlock(&icmp_lock);
    return result;
}

/* An ICMP error quoting one of our echo requests fails that request. */
void icmp_echo_error(const uint8_t *quote, size_t length, int error)
{
    if (length < 28 || quote[9] != 1 || quote[20] != 8)
        return;
    unsigned id = net_get_be16(quote + 24);
    if (id >= 0x4d00 && id < 0x4d00 + ECHO_SLOTS)
        echo_complete(id - 0x4d00, net_get_be16(quote + 26), -error, 0);
}

static uint64_t window;
static unsigned replies;
static bool allow_reply(void)
{
    uint64_t now = net_clock_ms();
    if (now < window || now - window >= 1000) {
        window = now;
        replies = 0;
    }
    if (replies == 20) {
        net_ip_stats.icmp_suppressed++;
        return false;
    }
    replies++;
    return true;
}

/* The caller has validated a local unicast IPv4 packet. Never emit an
 * error about an ICMP error, and quote only header plus eight bytes. */
void icmp_error(struct pbuf *original, uint8_t code)
{
    const uint8_t *ip = original->data;
    if (original->len < 20 || net_get_be32(ip + 16) == IPV4_BROADCAST)
        return;
    if (ip[9] == IPPROTO_ICMP && (original->len < 21 || (ip[20] != 8 && ip[20] != 0)))
        return;
    if (!allow_reply())
        return;
    struct pbuf *p = pbuf_alloc(PBUF_CONTROL);
    if (!p)
        return;
    size_t quote_length = MIN(original->len, 28);
    size_t message_length = 8 + quote_length;
    uint8_t *h = pbuf_put(p, message_length);
    memset(h, 0, 8);
    h[0] = 3;
    h[1] = code;
    memcpy(h + 8, ip, quote_length);
    net_put_be16(h + 2, net_checksum(h, message_length));
    net_ip_stats.icmp_errors++;
    ipv4_output(p, net_get_be32(ip + 16), net_get_be32(ip + 12), 1);
}

void icmp_input(struct netif *n, struct pbuf *p)
{
    uint8_t *h = p->data + 20;
    size_t len = p->len - 20;
    if (len < 8 || net_checksum(h, len)) {
        net_ip_stats.invalid++;
        goto out;
    }
    if (h[0] == 8 && h[1] == 0 && allow_reply()) {
        uint32_t src = net_get_be32(p->data + 12), dst = net_get_be32(p->data + 16);
        pbuf_pull(p, 20);
        h[0] = 0;
        h[2] = h[3] = 0;
        net_put_be16(h + 2, net_checksum(h, len));
        net_ip_stats.icmp_echo++;
        ipv4_output(p, dst, src, 1);
        return;
    }
    if (h[0] == 0 && h[1] == 0) {
        net_ip_stats.icmp_echo_reply++;
        unsigned id = net_get_be16(h + 4);
        if (id >= 0x4d00 && id < 0x4d00 + ECHO_SLOTS)
            echo_complete(id - 0x4d00, net_get_be16(h + 6), 0, p->data[8]);
    }
    if ((h[0] == 3 || h[0] == 11) && len >= 36 && !ipv4_validate_quote(h + 8, len - 8)) {
        net_ip_stats.pmtu_rejected++;
        goto out;
    }
    if (h[0] == 3 && h[1] == 4 && len >= 36) {
        ipv4_path_feedback(h + 8, len - 8, net_get_be16(h + 6));
        goto out;
    }
    if (h[0] == 3 && h[1] <= 15 && len >= 36) {
        int error;
        switch (h[1]) {
        case 0:
            error = ENETUNREACH;
            break;
        case 3:
            error = ECONNREFUSED;
            break;
        case 4:
            error = EMSGSIZE;
            break;
        default:
            error = EHOSTUNREACH;
            break;
        }
        udp_icmp_error(h + 8, len - 8, error);
        tcp_icmp_error(h + 8, len - 8, error);
        icmp_echo_error(h + 8, len - 8, error);
    } else if (h[0] == 11 && h[1] <= 1 && len >= 36) {
        udp_icmp_error(h + 8, len - 8, EHOSTUNREACH);
        tcp_icmp_error(h + 8, len - 8, EHOSTUNREACH);
    }
out:
    pbuf_free(p);
}
