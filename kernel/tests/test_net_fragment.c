/* Reassembly through production UDP delivery and bounded context recovery. */
#include <console.h>
#include <errno.h>
#include <net/worker.h>
#include <net/clock.h>
#include "net_helpers.h"

#define LOCAL 0x0a000002u
#define PEER 0x0a000001u
static int discard_output(struct netif *interface, struct pbuf *packet)
{
    pbuf_free(packet);
    return 0;
}
static const struct netif_ops operations = {
    .input = ethernet_input,
    .output = discard_output,
};
static struct netif interface = {
    .name = "frag0",
    .flags = NETIF_ETHERNET,
    .mtu = 1500,
    .ops = &operations,
};
static uint8_t datagram[3008];
static void fragment(unsigned id, unsigned offset, unsigned length, bool more)
{
    struct pbuf *packet = ip_packet(PEER, LOCAL, IPPROTO_UDP, length);
    memcpy(packet->data + 20, datagram + offset, length);
    net_put_be16(packet->data + 4, id);
    net_put_be16(packet->data + 6, offset / 8 | (more ? 0x2000 : 0));
    net_put_be16(packet->data + 10, 0);
    net_put_be16(packet->data + 10, net_checksum(packet->data, 20));
    ipv4_input(&interface, packet);
}
static void valid_datagram(unsigned id)
{
    fragment(id, 2048, sizeof datagram - 2048, false);
    fragment(id, 2048, sizeof datagram - 2048, false);
    fragment(id, 0, 1024, true);
    fragment(id, 1024, 1024, true);
}
static int checks(struct net_request *request)
{
    ktest_assert(netif_register(&interface) == 0, "fragment interface");
    netif_set_up(&interface, true);
    ktest_assert(net_configure(&interface, LOCAL, 0xffffff00u, 0) == 0, "fragment address");
    net_put_be16(datagram, 8001);
    net_put_be16(datagram + 2, 8000);
    net_put_be16(datagram + 4, sizeof datagram);
    for (unsigned i = 8; i < sizeof datagram; i++)
        datagram[i] = (uint8_t)(i * 37);
    net_put_be16(datagram + 6, udp_checksum(PEER, LOCAL, datagram, sizeof datagram));
    struct file *file;
    ktest_assert(socket_create(AF_INET, SOCK_DGRAM, 0, O_NONBLOCK, &file) == 0,
                 "fragment receiver");
    struct socket *socket = socket_from_file(file);
    struct sockaddr_storage name;
    address(&name, LOCAL, 8000);
    ktest_assert(socket_bind(socket, &name, 16) == 0, "fragment receiver bind");
    valid_datagram(1);
    static char received[3000];
    struct socket_msg message = {
        .data = received,
        .len = sizeof received,
    };
    ktest_assert(socket_recvmsg(socket, &message) == sizeof received &&
                     !memcmp(received, datagram + 8, sizeof received),
                 "reverse order and exact duplicate reconstruct one checked UDP datagram");
    ktest_assert(!net_ip_stats.reassembly_active, "completed context released");
    fragment(2, 0, 1024, true);
    fragment(2, 512, 1024, true);
    ktest_assert(net_ip_stats.fragment_invalid == 1, "overlap rejects full datagram");
    valid_datagram(2);
    ktest_assert(socket_recvmsg(socket, &message) == -EAGAIN,
                 "overlap quarantine prevents resurrection");
    ipv4_reassembly_flush(&interface);
    for (unsigned i = 0; i < IPV4_REASSEMBLY_SLOTS; i++)
        fragment(100 + i, 0, 1024, true);
    uint64_t full = net_ip_stats.fragment_full;
    fragment(200, 0, 1024, true);
    ktest_assert(net_ip_stats.fragment_full == full + 1 &&
                     net_ip_stats.reassembly_active == IPV4_REASSEMBLY_SLOTS,
                 "global reassembly contexts bounded");
    struct pbuf *oversized = ip_packet(PEER, LOCAL, IPPROTO_UDP, 8);
    net_put_be16(oversized->data + 6, 8191);
    net_put_be16(oversized->data + 10, 0);
    net_put_be16(oversized->data + 10, net_checksum(oversized->data, 20));
    ipv4_input(&interface, oversized);
    ktest_assert(net_ip_stats.fragment_invalid == 2, "oversized fragment rejected before copy");
    file_put(file);
    return 0;
}
static int recovery(struct net_request *request)
{
    ktest_assert(!net_ip_stats.reassembly_active, "actual worker expires missing fragments");
    uint64_t before = net_ip_stats.reassembled;
    valid_datagram(300);
    ktest_assert(net_ip_stats.reassembled == before + 1, "valid traffic recovers after pressure");
    net_configure(&interface, 0, 0, 0);
    netif_unregister(&interface);
    return 0;
}
static void test_fragment(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    net_clock_control(true);
    struct net_request request;
    net_request_init(&request, checks);
    ktest_assert(net_request_run(&request) == 0, "fragment checks");
    net_clock_advance(IPV4_REASSEMBLY_MS);
    net_worker_kick();
    net_worker_drain();
    net_request_init(&request, recovery);
    ktest_assert(net_request_run(&request) == 0, "fragment recovery");
    net_clock_control(false);
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(after.free == before.free, "fragment packet pool restored");
    kprintf("net_fragment: ok, contexts high-water %u/%u\n",
            net_ip_stats.reassembly_high_water,
            IPV4_REASSEMBLY_SLOTS);
}
KTEST_DEFINE("net_fragment", test_fragment);
