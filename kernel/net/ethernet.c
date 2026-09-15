#include <net/ipv4.h>
#include <net/byteorder.h>
#include <lib/string.h>
#include <errno.h>

static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};

void ethernet_input(struct netif *n, struct pbuf *p)
{
    if (p->len < 14 || (memcmp(p->data, n->hwaddr, 6) && memcmp(p->data, broadcast, 6)) ||
        (p->data[6] & 1))
        goto drop;
    uint16_t type = net_get_be16(p->data + 12);
    /* IPv4 to the broadcast MAC is accepted; ipv4_input keeps only the
     * limited broadcast UDP case and never answers it. Multicast stays out. */
    if (type == 0x0800) {
        pbuf_pull(p, 14);
        ipv4_input(n, p);
        return;
    }
    if (type == 0x0806) {
        /* ARP validates sender hardware against the enclosing Ethernet header. */
        if (p->len < 42 || memcmp(p->data + 6, p->data + 22, 6))
            goto drop;
        pbuf_pull(p, 14);
        arp_input(n, p);
        return;
    }
drop:
    atomic_u64_fetch_add_relaxed(&n->stats.rx_dropped, 1);
    pbuf_free(p);
}

int ethernet_output(struct netif *n, struct pbuf *p, const uint8_t *mac, uint16_t type)
{
    uint8_t *h = pbuf_push(p, 14);
    if (!h) {
        pbuf_free(p);
        return -EMSGSIZE;
    }
    memcpy(h, mac, 6);
    memcpy(h + 6, n->hwaddr, 6);
    net_put_be16(h + 12, type);
    if (p->len < 60) {
        size_t pad = 60 - p->len;
        void *tail = pbuf_put(p, pad);
        if (!tail) {
            pbuf_free(p);
            return -EMSGSIZE;
        }
        memset(tail, 0, pad);
    }
    return netif_output(n, p);
}
