/* UDP endpoint semantics and native host interoperability (N05). */
#include <tests/ktest.h>
#include <console.h>
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

#include <lib/cmdline.h>
#include "net_helpers.h"

static int corrupt_udp(struct net_request *r)
{
    struct pbuf *p = ip_packet(IPV4_LOOPBACK, IPV4_LOOPBACK, 17, 9);
    uint8_t *h = p->data + 20;
    net_put_be16(h, 8001);
    net_put_be16(h + 2, 8000);
    net_put_be16(h + 4, 9);
    h[8] = 0xa5;
    net_put_be16(h + 6, udp_checksum(IPV4_LOOPBACK, IPV4_LOOPBACK, h, 9));
    h[8] ^= 1;
    uint64_t before = net_ip_stats.udp_invalid;
    ipv4_input(netif_loopback(), p);
    ktest_assert(net_ip_stats.udp_invalid == before + 1, "invalid checksum dropped");
    p = ip_packet(IPV4_LOOPBACK, IPV4_LOOPBACK, 17, 9);
    h = p->data + 20;
    net_put_be16(h, 8001);
    net_put_be16(h + 2, 8000);
    net_put_be16(h + 4, 9);
    h[8] = 42;
    ipv4_input(netif_loopback(), p); /* IPv4 UDP checksum zero is legal */
    return 0;
}
static void test_udp(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    struct file *af, *bf, *cf;
    ktest_assert(socket_create(AF_INET, SOCK_DGRAM, 0, O_NONBLOCK, &af) == 0, "UDP create");
    ktest_assert(socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, O_NONBLOCK, &bf) == 0,
                 "explicit UDP");
    ktest_assert(socket_create(AF_INET, SOCK_DGRAM, 0, O_NONBLOCK, &cf) == 0, "third endpoint");
    struct socket *a = socket_from_file(af), *b = socket_from_file(bf), *c = socket_from_file(cf);
    struct sockaddr_storage name;
    address(&name, 0, 8000);
    ktest_assert(socket_bind(a, &name, 16) == 0, "wildcard bind");
    address(&name, IPV4_LOOPBACK, 8000);
    ktest_assert(socket_bind(b, &name, 16) == -EADDRINUSE, "wildcard conflicts with specific");
    address(&name, 0x0b000001, 8001);
    ktest_assert(socket_bind(b, &name, 16) == -EADDRNOTAVAIL, "nonlocal bind refused");
    address(&name, IPV4_LOOPBACK, 8001);
    ktest_assert(socket_bind(b, &name, 16) == 0, "specific bind");
    address(&name, IPV4_LOOPBACK + 1, 8001);
    ktest_assert(socket_bind(c, &name, 16) == 0, "same port distinct addresses");
    struct socket_msg empty = {
        .flags = MSG_DONTWAIT,
    };
    ktest_assert(socket_recvmsg(a, &empty) == -EAGAIN, "empty queue");
    ktest_assert(send_datagram(b, IPV4_LOOPBACK, 8000, NULL, 0) == 0, "send empty datagram");
    net_worker_drain();
    ktest_assert(a->ops->poll(a) & POLLIN, "empty datagram is readable");
    ktest_assert(file_read(af, NULL, 0) == 0 && (a->ops->poll(a) & POLLIN),
                 "zero read does not consume");
    ktest_assert(socket_recvmsg(a, &empty) == 0 && !(a->ops->poll(a) & POLLIN),
                 "recv consumes empty datagram");
    char text[] = "abcdef", buffer[16];
    ktest_assert(send_datagram(b, IPV4_LOOPBACK, 8000, text, 6) == 6, "send payload");
    net_worker_drain();
    struct socket_msg m = {
        .data = buffer,
        .len = 2,
        .addr = &name,
        .flags = MSG_PEEK | MSG_TRUNC,
    };
    ktest_assert(socket_recvmsg(a, &m) == 6 && m.rflags == MSG_TRUNC && !memcmp(buffer, "ab", 2),
                 "peek returns full datagram length");
    ktest_assert(m.addrlen == 16 && ntohs(((struct sockaddr_in *)&name)->sin_port) == 8001,
                 "source address preserved");
    m.flags = 0;
    m.len = sizeof buffer;
    ktest_assert(socket_recvmsg(a, &m) == 6 && !memcmp(buffer, text, 6), "peek retained datagram");
    address(&name, IPV4_LOOPBACK, 8001);
    ktest_assert(socket_connect(a, &name, 16) == 0, "connect filter");
    ktest_assert(send_datagram(c, IPV4_LOOPBACK, 8000, text, 6) == 6, "send from wrong peer");
    net_worker_drain();
    ktest_assert(socket_recvmsg(a, &empty) == -EAGAIN, "wrong peer filtered");
    for (unsigned i = 0; i < 20; i++)
        ktest_assert(send_datagram(b, IPV4_LOOPBACK, 8000, NULL, 0) == 0, "fill receive ring");
    net_worker_drain();
    for (unsigned i = 0; i < 16; i++)
        ktest_assert(socket_recvmsg(a, &empty) == 0, "queued empty datagram %u", i);
    ktest_assert(socket_recvmsg(a, &empty) == -EAGAIN && net_ip_stats.udp_full >= 4,
                 "bounded receive ring");
    struct net_request r;
    net_request_init(&r, corrupt_udp);
    net_request_run(&r);
    m.len = sizeof buffer;
    m.flags = MSG_DONTWAIT;
    ktest_assert(socket_recvmsg(a, &m) == 1 && buffer[0] == 42, "zero checksum accepted");
    ktest_assert(send_datagram(b, IPV4_LOOPBACK, 8000, text, IPV4_MAX_PACKET) == -EMSGSIZE,
                 "oversize refused atomically");
    /* A connected sender observes a port-unreachable error once, without
     * confusing it with the successful enqueue of the original datagram. */
    address(&name, IPV4_LOOPBACK, 8999);
    ktest_assert(socket_connect(b, &name, 16) == 0, "connect to unused port");
    struct socket_msg outbound = {.data = text, .len = 6};
    ktest_assert(socket_sendmsg(b, &outbound) == 6, "enqueue before asynchronous refusal");
    net_worker_drain();
    net_worker_drain();
    ktest_assert(b->ops->poll(b) & POLLERR, "ICMP error wakes poll");
    int error = 0;
    socklen_t error_length = sizeof error;
    ktest_assert(socket_getsockopt(b, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 &&
                     error == ECONNREFUSED,
                 "ICMP port unreachable maps to ECONNREFUSED");
    error_length = sizeof error;
    ktest_assert(socket_getsockopt(b, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 &&
                     error == 0,
                 "SO_ERROR consumed exactly once");

    ktest_assert(socket_sendmsg(b, &outbound) == 6, "enqueue another refused datagram");
    net_worker_drain();
    net_worker_drain();
    ktest_assert(socket_recvmsg(b, &empty) == -ECONNREFUSED,
                 "receive returns a negative asynchronous errno");

    /* Closing with unread packets releases the entire receive ring. */
    for (unsigned i = 0; i < 8; i++)
        ktest_assert(send_datagram(b, IPV4_LOOPBACK, 8000, text, 6) == 6,
                     "queue data before receiver close");
    net_worker_drain();
    file_put(af);
    file_put(bf);
    file_put(cf);
    net_worker_drain();
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(after.free == before.free, "UDP pool restored %u/%u", after.free, before.free);
    kprintf("net_udp: ok\n");
}
KTEST_DEFINE("net_udp", test_udp);

static void test_udp_peer(void)
{
    char port_text[16];
    ktest_assert(cmdline_lookup("netpeer_port", port_text, sizeof port_text), "host port supplied");
    char *end;
    unsigned long long port = strtoull(port_text, &end, 10);
    ktest_assert(end != port_text && *end == '\0', "numeric port");
    ktest_assert(port && port < 65536, "port range");
    struct netif *n = netif_find("eth0");
    ktest_assert(n, "NIC present");
    ktest_assert(net_configure(n, 0x0a00020f, 0xffffff00u, 0x0a000202) == 0,
                 "QEMU user network static config");
    struct file *f;
    ktest_assert(socket_create(AF_INET, SOCK_DGRAM, 0, O_NONBLOCK, &f) == 0, "UDP endpoint");
    struct socket *s = socket_from_file(f);
    struct sockaddr_storage a;
    address(&a, 0x0a000202, port);
    ktest_assert(socket_connect(s, &a, 16) == 0, "connect host echo socket");
    static char data[8100], received[8100];
    for (unsigned round = 0; round < 300; round++) {
        static const size_t sizes[] = {0, 1472, 37, 6000};
        size_t len = sizes[round % 4];
        for (unsigned j = 0; j < len; j++)
            data[j] = (char)(round + j);
        struct socket_msg m = {
            .data = data,
            .len = len,
        };
        ktest_assert(socket_sendmsg(s, &m) == (long)len, "host send round %u", round);
        long result;
        uint64_t deadline = timer_ms() + 3000;
        struct socket_msg in = {
            .data = received,
            .len = sizeof received,
            .addr = &a,
        };
        do {
            result = socket_recvmsg(s, &in);
            if (result != -EAGAIN)
                break;
            sleep_ms(1);
        } while (timer_ms() < deadline);
        ktest_assert(result == (long)len && !memcmp(data, received, len),
                     "native host echo %u: %ld/%lu",
                     round,
                     result,
                     len);
        ktest_assert(in.addrlen == 16 && ntohs(((struct sockaddr_in *)&a)->sin_port) == port,
                     "native host source");
    }
    struct socket_msg m = {
        .data = data,
        .len = IPV4_MAX_PACKET,
    };
    ktest_assert(socket_sendmsg(s, &m) == -EMSGSIZE, "bounded datagram limit");
    file_put(f);
    kprintf("net_udp_peer: 300 native host echoes (empty, odd, MTU), ok\n");
}
KTEST_DEFINE("net_udp_peer", test_udp_peer);
