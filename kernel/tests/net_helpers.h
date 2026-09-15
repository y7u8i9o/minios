#pragma once
/* Kernel-owned packet and socket-address builders shared by the network
 * tests. They only prepare input; production code performs all parsing,
 * routing, checksum validation, and endpoint delivery. */
#include <tests/ktest.h>
#include <net/ipv4.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <ipc/socket.h>
#include <lib/string.h>

static inline struct pbuf *ip_packet(uint32_t src, uint32_t dst, uint8_t protocol, size_t payload)
{
    struct pbuf *p = pbuf_alloc(PBUF_DATA);
    ktest_assert(p, "allocate packet");
    uint8_t *h = pbuf_put(p, 20 + payload);
    memset(h, 0, 20 + payload);
    h[0] = 0x45;
    h[8] = 64;
    h[9] = protocol;
    net_put_be16(h + 2, (uint16_t)p->len);
    net_put_be32(h + 12, src);
    net_put_be32(h + 16, dst);
    net_put_be16(h + 10, net_checksum(h, 20));
    return p;
}
static inline struct pbuf *payload(void)
{
    struct pbuf *p = pbuf_alloc(PBUF_DATA);
    memset(pbuf_put(p, 8), 0, 8);
    return p;
}

static inline void address(struct sockaddr_storage *s, uint32_t ip, uint16_t port)
{
    memset(s, 0, sizeof *s);
    struct sockaddr_in *a = (void *)s;
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(ip);
    a->sin_port = htons(port);
}
static inline long send_datagram(
    struct socket *s, uint32_t ip, uint16_t port, char *data, size_t len)
{
    struct sockaddr_storage a;
    address(&a, ip, port);
    struct socket_msg m = {
        .addr = &a,
        .addrlen = sizeof(struct sockaddr_in),
        .data = data,
        .len = len,
    };
    return socket_sendmsg(s, &m);
}
