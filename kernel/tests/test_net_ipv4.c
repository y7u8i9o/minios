/* Deterministic N03–N05 tests use production parsers and worker requests.
 * Separate live NIC cases exercise the actual virtqueue and controlled peer. */
#include <tests/ktest.h>
#include <console.h>
#include <lib/cmdline.h>
#include <net/ipv4.h>
#include <net/worker.h>
#include <net/clock.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <ipc/socket.h>
#include <drivers/virtio/virtio.h>
#include <drivers/virtio/virtio_net.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <errno.h>

#include "net_helpers.h"

#define LOCAL 0x0a000002u
#define PEER 0x0a000001u
static uint8_t frames[128][1600];
static unsigned lengths[128], captured;
static const uint8_t peer_mac[6] = {2, 0, 0, 0, 0, 1};
static int capture(struct netif *n, struct pbuf *p)
{
    if (captured < 128) {
        lengths[captured] = p->len;
        memcpy(frames[captured++], p->data, MIN(p->len, 1600));
    }
    pbuf_free(p);
    return 0;
}
static const struct netif_ops fake_ops = {
    .output = capture,
    .input = ethernet_input,
};
static struct netif fake = {
    .name = "testnet",
    .flags = NETIF_ETHERNET,
    .mtu = 1500,
    .hwaddr = {2, 0, 0, 0, 0, 2},
    .ops = &fake_ops,
};

static void ip_rechecksum(struct pbuf *p)
{
    p->data[10] = p->data[11] = 0;
    net_put_be16(p->data + 10, net_checksum(p->data, (p->data[0] & 15) * 4));
}
static void arp_reply(uint32_t sender)
{
    struct pbuf *p = pbuf_alloc(PBUF_CONTROL);
    uint8_t *h = pbuf_put(p, 28);
    memset(h, 0, 28);
    net_put_be16(h, 1);
    net_put_be16(h + 2, 0x0800);
    h[4] = 6;
    h[5] = 4;
    net_put_be16(h + 6, 2);
    memcpy(h + 8, peer_mac, 6);
    net_put_be32(h + 14, sender);
    memcpy(h + 18, fake.hwaddr, 6);
    net_put_be32(h + 24, LOCAL);
    arp_input(&fake, p);
}
static int ipv4_checks(struct net_request *r)
{
    ktest_assert(netif_register(&fake) == 0, "fake interface");
    netif_set_up(&fake, true);
    ktest_assert(net_configure(&fake, LOCAL, 0xff00ff00u, 0) == -EINVAL, "invalid mask");
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0x0b000001) == -EINVAL,
                 "offlink gateway");
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, PEER) == 0, "static config");
    struct net_route route;
    ktest_assert(net_route_lookup(0x0b000001, &route) == 0 && route.next_hop == PEER,
                 "gateway route");
    ktest_assert(net_route_lookup(LOCAL, &route) == 0 && route.netif == netif_loopback(),
                 "own address stays local");
    captured = 0;
    ktest_assert(ipv4_output(payload(), 0, 0x0b000001, 17) == 0, "queue off-subnet packet");
    ktest_assert(captured == 1 && net_get_be16(frames[0] + 12) == 0x0806 &&
                     net_get_be32(frames[0] + 38) == PEER,
                 "ARP resolves gateway, not destination");
    arp_reply(PEER);
    ktest_assert(captured == 2 && !memcmp(frames[1], peer_mac, 6) &&
                     net_get_be32(frames[1] + 30) == 0x0b000001,
                 "gateway MAC and remote IP");
    struct pbuf *p = ip_packet(PEER, LOCAL, 1, 9);
    p->data[20] = 8;
    p->data[28] = 0xa5;
    net_put_be16(p->data + 22, net_checksum(p->data + 20, 9));
    memset(pbuf_put(p, 15), 0xff, 15); /* Ethernet padding is trimmed */
    ipv4_input(&fake, p);
    ktest_assert(captured == 3 && frames[2][34] == 0 && frames[2][42] == 0xa5 &&
                     net_checksum(frames[2] + 34, 9) == 0,
                 "odd ICMP echo with padding");
    uint64_t invalid = net_ip_stats.invalid, fragments = net_ip_stats.fragments;
    for (int kind = 0; kind < 7; kind++) {
        p = ip_packet(PEER, LOCAL, 1, 8);
        if (kind == 0)
            p->data[0] = 0x65;
        if (kind == 1)
            p->data[10] ^= 1;
        if (kind == 2)
            net_put_be16(p->data + 2, 2000);
        if (kind == 3)
            net_put_be16(p->data + 2, 19);
        if (kind == 4) {
            p->data[6] = 0x20;
            ip_rechecksum(p);
        }
        if (kind == 5) {
            net_put_be16(p->data + 4, 1);
            p->data[7] = 1;
            ip_rechecksum(p);
        }
        if (kind == 6) {
            p->data[8] = 0;
            ip_rechecksum(p);
        }
        ipv4_input(&fake, p);
    }
    ktest_assert(net_ip_stats.invalid == invalid + 5 && net_ip_stats.fragments == fragments + 2,
                 "invalid headers/fragments counted");
    p = ip_packet(PEER, LOCAL, 17, 8);
    p->data[0] = 0x46;
    ip_rechecksum(p);
    uint64_t options = net_ip_stats.options;
    ipv4_input(&fake, p);
    ktest_assert(net_ip_stats.options == options + 1, "options explicitly refused");
    p = pbuf_alloc(PBUF_DATA);
    pbuf_put(p, 1481);
    ktest_assert(ipv4_output(p, 0, PEER, IPPROTO_TCP) == -EMSGSIZE, "DF MTU checked before ARP");
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0) == 0, "remove default route");
    ktest_assert(ipv4_output(payload(), 0, 0x0b000001, 17) == -ENETUNREACH, "missing route");
    return 0;
}
static int arp_fill(struct net_request *r)
{
    captured = 0;
    for (unsigned i = 0; i < 32; i++)
        ktest_assert(
            ipv4_output(payload(), 0, PEER + (i / 4) * 4, 17) == 0, "bounded pending %u", i);
    ktest_assert(ipv4_output(payload(), 0, PEER, 17) == -ENOBUFS, "neighbor limit");
    ktest_assert(ipv4_output(payload(), 0, PEER + 80, 17) == -ENOBUFS, "global limit");
    return 0;
}
static int cleanup_fake(struct net_request *r)
{
    net_configure(&fake, 0, 0, 0);
    netif_unregister(&fake);
    return 0;
}
static void test_ipv4(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    struct net_request r;
    net_request_init(&r, ipv4_checks);
    ktest_assert(net_request_run(&r) == 0, "IPv4 checks");
    net_clock_control(true);
    net_request_init(&r, arp_fill);
    ktest_assert(net_request_run(&r) == 0, "ARP fill");
    for (unsigned i = 0; i < 3; i++) {
        net_clock_advance(1000);
        net_worker_kick();
        net_worker_drain();
    }
    ktest_assert(net_ip_stats.arp_timeouts == 8 && captured == 24,
                 "three attempts then eight timeouts: %lu/%u",
                 net_ip_stats.arp_timeouts,
                 captured);
    net_request_init(&r, cleanup_fake);
    net_request_run(&r);
    net_clock_control(false);
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(
        after.free == before.free, "ARP timeout restores pool %u/%u", after.free, before.free);
    kprintf("net_ipv4: ok\n");
}
KTEST_DEFINE("net_ipv4", test_ipv4);

static int ping_peer(struct net_request *r)
{
    struct pbuf *p = payload();
    p->data[0] = 8;
    net_put_be16(p->data + 4, 0x1234);
    net_put_be16(p->data + 2, net_checksum(p->data, p->len));
    return ipv4_output(p, 0, PEER, 1);
}
static int read_icmp_counts(struct net_request *r)
{
    return net_ip_stats.icmp_echo_reply && net_ip_stats.icmp_echo ? 1 : 0;
}
static void test_icmp_peer(void)
{
    struct netif *n = netif_find("eth0");
    ktest_assert(n, "NIC present");
    ktest_assert(net_configure(n, LOCAL, 0xffffff00u, PEER) == 0, "static Ethernet config");
    struct net_request r;
    net_request_init(&r, ping_peer);
    ktest_assert(net_request_run(&r) == 0, "echo queued via ARP");
    uint64_t deadline = timer_ms() + 5000;
    int counts = 0;
    while (timer_ms() < deadline) {
        net_request_init(&r, read_icmp_counts);
        counts = net_request_run(&r);
        if (counts)
            break;
        sleep_ms(5);
    }
    ktest_assert(counts, "host and guest each answer ICMP echo");
    kprintf("net_icmp: bidirectional peer echo, ok\n");
}
KTEST_DEFINE("net_icmp", test_icmp_peer);

static int prepare_arp_failure(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "register ARP test interface");
    netif_set_up(&fake, true);
    captured = 0;
    return net_configure(&fake, LOCAL, 0xffffff00u, 0);
}

static void test_arp_failure(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    struct net_request request;
    net_request_init(&request, prepare_arp_failure);
    ktest_assert(net_request_run(&request) == 0, "configure ARP test link");
    net_clock_control(true);

    struct file *file;
    ktest_assert(socket_create(AF_INET, SOCK_DGRAM, 0, O_NONBLOCK, &file) == 0,
                 "create connected UDP endpoint");
    struct socket *socket = socket_from_file(file);
    struct sockaddr_storage peer;
    address(&peer, PEER, 9000);
    ktest_assert(socket_connect(socket, &peer, sizeof(struct sockaddr_in)) == 0,
                 "connect on unresolved link");
    struct socket_msg message = {.data = "arp", .len = 3};
    ktest_assert(socket_sendmsg(socket, &message) == 3, "accept packet pending resolution");
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        net_clock_advance(1000);
        net_worker_kick();
        net_worker_drain();
    }
    int error = 0;
    socklen_t length = sizeof error;
    ktest_assert(captured == 3, "exactly three ARP attempts");
    ktest_assert(socket_getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                     error == EHOSTUNREACH,
                 "neighbor timeout reaches connected socket");

    ktest_assert(socket_sendmsg(socket, &message) == 3, "queue packet before link down");
    netif_set_up(&fake, false);
    net_clock_advance(1000);
    net_worker_kick();
    net_worker_drain();
    length = sizeof error;
    ktest_assert(socket_getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                     error == ENETDOWN,
                 "link down fails pending resolution");
    file_put(file);
    net_request_init(&request, cleanup_fake);
    net_request_run(&request);
    net_clock_control(false);
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(after.free == before.free, "ARP failures release queued packets");
    kprintf("net_arp: timeout and link-down errors, ok\n");
}
KTEST_DEFINE("net_arp", test_arp_failure);

/* The N16 checks drive RFC 5227 probes and announcements with injected ARP
 * packets. The interface has no address, as during DHCP, and conflict
 * detection must work regardless. */
static void arp_packet(uint16_t op, const uint8_t *mac, uint32_t sender, uint32_t target)
{
    struct pbuf *p = pbuf_alloc(PBUF_CONTROL);
    uint8_t *h = pbuf_put(p, 28);
    memset(h, 0, 28);
    net_put_be16(h, 1);
    net_put_be16(h + 2, 0x0800);
    h[4] = 6;
    h[5] = 4;
    net_put_be16(h + 6, op);
    memcpy(h + 8, mac, 6);
    net_put_be32(h + 14, sender);
    if (op == 2)
        memcpy(h + 18, fake.hwaddr, 6);
    net_put_be32(h + 24, target);
    arp_input(&fake, p);
}
static void expect_probe(unsigned index, uint32_t sender, uint32_t target)
{
    const uint8_t *f = frames[index];
    static const uint8_t zero[6], all[6] = {255, 255, 255, 255, 255, 255};
    ktest_assert(lengths[index] >= 42 && !memcmp(f, all, 6) && net_get_be16(f + 12) == 0x0806 &&
                     net_get_be16(f + 20) == 1 && !memcmp(f + 22, fake.hwaddr, 6) &&
                     net_get_be32(f + 28) == sender && !memcmp(f + 32, zero, 6) &&
                     net_get_be32(f + 38) == target,
                 "ARP frame %u: broadcast request from %x for %x",
                 index,
                 sender,
                 target);
}
static int probe_checks(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "register probe interface");
    netif_set_up(&fake, true);
    captured = 0;
    const uint32_t wanted = 0x0a000063;
    const uint8_t other[6] = {2, 0, 0, 0, 0, 9};
    uint8_t mac[6];
    uint64_t probes = net_ip_stats.arp_probes, conflicts = net_ip_stats.arp_conflicts;

    int slot = arp_probe_start(&fake, wanted, false);
    ktest_assert(slot >= 0 && captured == 1, "probe sent");
    expect_probe(0, 0, wanted);
    arp_packet(2, other, 0x0a000064, 0);
    arp_packet(1, fake.hwaddr, 0, wanted);
    ktest_assert(arp_probe_finish(slot, 0, mac) == 0,
                 "unrelated ARP and our own probe are no conflict");

    slot = arp_probe_start(&fake, wanted, false);
    arp_packet(2, other, wanted, 0);
    ktest_assert(arp_probe_finish(slot, 0, mac) == -EADDRINUSE && !memcmp(mac, other, 6),
                 "a reply from the address is a conflict");
    slot = arp_probe_start(&fake, wanted, false);
    arp_packet(1, other, 0, wanted);
    ktest_assert(arp_probe_finish(slot, 0, mac) == -EADDRINUSE,
                 "another host probing for the address is a conflict");
    slot = arp_probe_start(&fake, wanted, false);
    arp_packet(1, other, wanted, 0x0a000001);
    ktest_assert(arp_probe_finish(slot, 0, mac) == -EADDRINUSE,
                 "a request sent from the address is a conflict");

    int first = arp_probe_start(&fake, wanted, false);
    int second = arp_probe_start(&fake, wanted + 1, false);
    ktest_assert(first >= 0 && second >= 0 && arp_probe_start(&fake, wanted + 2, false) == -EBUSY,
                 "two probe slots");
    arp_packet(2, other, wanted + 1, 0);
    ktest_assert(arp_probe_finish(first, 0, mac) == 0 &&
                     arp_probe_finish(second, 0, mac) == -EADDRINUSE,
                 "a conflict marks only the slot of its address");

    unsigned before = captured;
    slot = arp_probe_start(&fake, wanted, true);
    expect_probe(before, wanted, wanted);
    ktest_assert(arp_probe_finish(slot, 0, mac) == 0 && net_ip_stats.arp_announcements >= 1,
                 "announcement sent with the address as sender and target");
    ktest_assert(net_ip_stats.arp_probes == probes + 6 &&
                     net_ip_stats.arp_conflicts == conflicts + 4,
                 "probe and conflict counters");
    ktest_assert(arp_probe_start(netif_loopback(), wanted, false) == -EOPNOTSUPP &&
                     arp_probe_start(&fake, 0, false) == -EINVAL,
                 "probes need an Ethernet interface and a unicast address");
    return cleanup_fake(request);
}
static void test_arp_probe(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    struct net_request request;
    net_request_init(&request, probe_checks);
    ktest_assert(net_request_run(&request) == 0, "probe checks");
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(after.free == before.free, "probe packets released");
    kprintf("net_arp_probe: probes, announcements and conflicts, ok\n");
}
KTEST_DEFINE("net_arp_probe", test_arp_probe);
