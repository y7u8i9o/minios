#include <net/ipv4.h>
#include <net/wire.h>
#include <net/tcp.h>
#include <net/net.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <lib/string.h>
#include <errno.h>

struct ipv4_stats net_ip_stats;
static uint16_t identification;

void ipv4_init(void)
{
    udp_init();
    tcp_init();
}

void ipv4_input(struct netif *n, struct pbuf *p)
{
    const uint8_t *h = p->data;
    unsigned total;
    if (!ipv4_header_valid(h, p->len, &total))
        goto invalid;
    unsigned ihl = (h[0] & 15) * 4;
    /* No IP options in this restricted stage, including source routes. */
    if (ihl != 20) {
        net_ip_stats.options++;
        goto drop;
    }
    uint32_t src = net_get_be32(h + 12);
    uint32_t dst = net_get_be32(h + 16);
    /* Limited broadcast is accepted on Ethernet for UDP only (DHCP); it is
     * never answered with ICMP and never reaches TCP. */
    bool broadcast = dst == IPV4_BROADCAST && (n->flags & NETIF_ETHERNET) && h[9] == 17;
    if (!ipv4_unicast(src) || (!ipv4_local(dst) && !broadcast))
        goto wrong_destination;

    if (n->flags & NETIF_ETHERNET) {
        /* Never route a wire packet into the loopback address range. A
         * directed broadcast source is invalid on this attached subnet;
         * host bits of a remote source cannot be interpreted using our mask. */
        if ((src >> 24) == 127 || (!broadcast && dst != ipv4_address(n)))
            goto wrong_destination;
        uint32_t mask = ipv4_netmask(n);
        uint32_t host = src & ~mask;
        bool on_link = (src & mask) == (ipv4_address(n) & mask);
        if (on_link && (!host || host == ~mask))
            goto wrong_destination;
    }
    pbuf_trim(p, total); /* remove Ethernet padding before upper-layer checks */
    if (net_get_be16(h + 6) & 0x3fff) {
        net_ip_stats.fragments++;
        p = ipv4_reassemble(n, p);
        if (!p)
            return;
        h = p->data;
    }
    if (h[9] == 1) {
        icmp_input(n, p);
        return;
    }
    if (h[9] == IPPROTO_TCP) {
        tcp_input(n, p);
        return;
    }
    if (h[9] == 17) {
        udp_input(n, p);
        return;
    }
    icmp_error(p, 2); /* protocol unreachable */
    goto drop;
wrong_destination:
    net_ip_stats.wrong_destination++;
    goto drop;
invalid:
    net_ip_stats.invalid++;
drop:
    net_ip_discard(p);
}

int ipv4_output(struct pbuf *p, uint32_t src, uint32_t dst, uint8_t protocol)
{
    struct net_route route;
    int error = net_route_lookup(dst, &route);
    if (error < 0) {
        pbuf_free(p);
        return error;
    }
    if (!src)
        src = route.source;
    /* An unconfigured interface sends limited broadcasts from 0.0.0.0. */
    bool unnumbered = !src && dst == IPV4_BROADCAST && protocol == IPPROTO_UDP;
    if ((!ipv4_local(src) && !unnumbered) ||
        (route.netif != netif_loopback() && (src >> 24) == 127)) {
        pbuf_free(p);
        return -EADDRNOTAVAIL;
    }
    if (p->len + 20 > IPV4_MAX_PACKET ||
        (protocol != IPPROTO_UDP && p->len + 20 > ipv4_path_mtu(dst, route.netif->mtu))) {
        net_ip_stats.too_big++;
        pbuf_free(p);
        return -EMSGSIZE;
    }
    uint8_t *h = pbuf_push(p, 20);
    if (!h) {
        pbuf_free(p);
        return -EMSGSIZE;
    }
    memset(h, 0, 20);
    h[0] = 0x45;
    h[8] = 64;
    h[9] = protocol;
    net_put_be16(h + 2, (uint16_t)p->len);
    net_put_be16(h + 4, identification++);
    net_put_be16(h + 6, protocol == IPPROTO_UDP ? 0 : 0x4000);
    net_put_be32(h + 12, src);
    net_put_be32(h + 16, dst);
    net_put_be16(h + 10, net_checksum(h, 20));
    ipv4_note_output(p);
    return route.netif == netif_loopback() ? netif_output(route.netif, p)
                                           : arp_output(route.netif, p, route.next_hop);
}
