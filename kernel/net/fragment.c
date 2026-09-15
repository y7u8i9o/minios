/* IPv4 reassembly is worker-owned and bounded by slots, fragments, bytes and
 * a non-renewable deadline. A conflicting overlap quarantines the full key
 * until expiry, preventing a later fragment from resurrecting a mixed packet. */
#include <net/ipv4.h>
#include <net/wire.h>
#include <net/worker.h>
#include <net/clock.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <lib/string.h>
#include <errno.h>

#define FRAGMENT_PIECES 64
#define FRAGMENT_PAYLOAD (IPV4_MAX_PACKET - IPV4_HEADER)
struct fragment_piece {
    uint16_t offset, length;
    bool more;
};
struct reassembly {
    struct netif *interface;
    struct pbuf *packet;
    uint32_t source, destination;
    uint16_t identification;
    uint8_t protocol;
    bool poisoned, first, last;
    unsigned end, highest, covered, pieces;
    uint64_t expires;
    uint8_t present[(FRAGMENT_PAYLOAD + 7) / 8];
    struct fragment_piece piece[FRAGMENT_PIECES];
};
static struct reassembly slots[IPV4_REASSEMBLY_SLOTS];
static struct net_timer timer;
static bool initialized;

static void release(struct reassembly *slot)
{
    if (slot->packet)
        pbuf_free(slot->packet);
    memset(slot, 0, sizeof *slot);
    net_ip_stats.reassembly_active--;
}
static void schedule(void);
static void expire(struct net_timer *unused)
{
    uint64_t now = net_clock_ms();
    for (unsigned i = 0; i < IPV4_REASSEMBLY_SLOTS; i++) {
        if (slots[i].interface && slots[i].expires <= now) {
            release(&slots[i]);
            net_ip_stats.fragment_expired++;
        }
    }
    schedule();
}
static void schedule(void)
{
    if (!initialized) {
        net_timer_init(&timer, expire);
        initialized = true;
    }
    uint64_t next = UINT64_MAX;
    for (unsigned i = 0; i < IPV4_REASSEMBLY_SLOTS; i++) {
        if (slots[i].interface)
            next = MIN(next, slots[i].expires);
    }
    if (next == UINT64_MAX)
        net_timer_cancel(&timer);
    else
        net_timer_arm(&timer, next);
}
void ipv4_reassembly_flush(struct netif *interface)
{
    for (unsigned i = 0; i < IPV4_REASSEMBLY_SLOTS; i++) {
        if (slots[i].interface == interface)
            release(&slots[i]);
    }
    schedule();
}
static void poison(struct reassembly *slot)
{
    slot->poisoned = true;
    if (slot->packet) {
        pbuf_free(slot->packet);
        slot->packet = NULL;
    }
    net_ip_stats.fragment_invalid++;
}
struct pbuf *ipv4_reassemble(struct netif *interface, struct pbuf *packet)
{
    uint8_t *header = packet->data;
    unsigned flags = net_get_be16(header + 6);
    unsigned offset = (flags & 8191) * 8;
    unsigned length = packet->len - 20;
    bool more = flags & 0x2000;
    if (!ipv4_fragment_bounds(flags, length, FRAGMENT_PAYLOAD, &offset)) {
        net_ip_stats.fragment_invalid++;
        pbuf_free(packet);
        return NULL;
    }
    uint32_t source = net_get_be32(header + 12);
    uint32_t destination = net_get_be32(header + 16);
    uint16_t id = net_get_be16(header + 4);
    struct reassembly *slot = NULL, *free_slot = NULL;
    for (unsigned i = 0; i < IPV4_REASSEMBLY_SLOTS; i++) {
        struct reassembly *candidate = &slots[i];
        if (candidate->interface && candidate->expires <= net_clock_ms()) {
            release(candidate);
            net_ip_stats.fragment_expired++;
        }
        if (!candidate->interface)
            free_slot = candidate;
        else if (candidate->interface == interface && candidate->source == source &&
                 candidate->destination == destination && candidate->identification == id &&
                 candidate->protocol == header[9])
            slot = candidate;
    }
    if (!slot) {
        struct pbuf *storage = free_slot ? pbuf_alloc(PBUF_DATA) : NULL;
        if (!storage) {
            net_ip_stats.fragment_full++;
            pbuf_free(packet);
            return NULL;
        }
        slot = free_slot;
        slot->interface = interface;
        slot->source = source;
        slot->destination = destination;
        slot->identification = id;
        slot->protocol = header[9];
        slot->packet = storage;
        slot->expires = net_clock_ms() + IPV4_REASSEMBLY_MS;
        pbuf_put(storage, IPV4_MAX_PACKET);
        net_ip_stats.reassembly_active++;
        net_ip_stats.reassembly_high_water =
            MAX(net_ip_stats.reassembly_high_water, net_ip_stats.reassembly_active);
        schedule();
    }
    if (slot->poisoned)
        goto incomplete;
    for (unsigned i = 0; i < slot->pieces; i++) {
        struct fragment_piece *piece = &slot->piece[i];
        if (piece->offset == offset && piece->length == length && piece->more == more &&
            !memcmp(slot->packet->data + 20 + offset, header + 20, length))
            goto incomplete; /* Exact duplicate has no effect on deadline. */
    }
    if (slot->pieces == FRAGMENT_PIECES || (slot->last && offset + length > slot->end) ||
        (!more &&
         ((slot->last && slot->end != offset + length) || slot->highest > offset + length))) {
        poison(slot);
        goto incomplete;
    }
    for (unsigned i = offset; i < offset + length; i++) {
        if (slot->present[i / 8] & (1u << (i % 8))) {
            poison(slot);
            goto incomplete;
        }
    }
    for (unsigned i = offset; i < offset + length; i++)
        slot->present[i / 8] |= 1u << (i % 8);
    slot->piece[slot->pieces++] = (struct fragment_piece){offset, length, more};
    memcpy(slot->packet->data + 20 + offset, header + 20, length);
    slot->covered += length;
    slot->highest = MAX(slot->highest, offset + length);
    if (!offset) {
        memcpy(slot->packet->data, header, 20);
        slot->first = true;
    }
    if (!more) {
        slot->last = true;
        slot->end = offset + length;
    }
    if (slot->first && slot->last && slot->covered == slot->end) {
        struct pbuf *result = slot->packet;
        slot->packet = NULL;
        pbuf_trim(result, 20 + slot->end);
        net_put_be16(result->data + 2, result->len);
        net_put_be16(result->data + 6, 0);
        net_put_be16(result->data + 10, 0);
        net_put_be16(result->data + 10, net_checksum(result->data, 20));
        release(slot);
        schedule();
        pbuf_free(packet);
        net_ip_stats.reassembled++;
        return result;
    }
incomplete:
    pbuf_free(packet);
    return NULL;
}

/* ARP queues the complete datagram. Fragmentation starts only after neighbor
 * resolution, so a large datagram does not consume the small pending queue
 * with individual fragments. All fragment buffers are reserved before output. */
int ipv4_link_output(struct netif *interface, struct pbuf *packet, const uint8_t *mac)
{
    unsigned mtu = ipv4_path_mtu(net_get_be32(packet->data + 16), interface->mtu);
    if (packet->len <= mtu)
        return ethernet_output(interface, packet, mac, 0x0800);
    if ((net_get_be16(packet->data + 6) & 0x4000) || mtu < 68) {
        pbuf_free(packet);
        return -EMSGSIZE;
    }
    unsigned unit = (mtu - 20) & ~7u;
    unsigned payload = packet->len - 20;
    unsigned count = (payload + unit - 1) / unit;
    if (count > FRAGMENT_PIECES) {
        pbuf_free(packet);
        return -EMSGSIZE;
    }
    struct pbuf *fragments[FRAGMENT_PIECES];
    unsigned allocated = 0;
    for (; allocated < count; allocated++) {
        fragments[allocated] = pbuf_alloc(PBUF_DATA);
        if (!fragments[allocated])
            break;
        unsigned offset = allocated * unit;
        unsigned length = MIN(unit, payload - offset);
        uint8_t *header = pbuf_put(fragments[allocated], 20 + length);
        memcpy(header, packet->data, 20);
        memcpy(header + 20, packet->data + 20 + offset, length);
        net_put_be16(header + 2, length + 20);
        net_put_be16(header + 6, offset / 8 | (allocated + 1 < count ? 0x2000 : 0));
        net_put_be16(header + 10, 0);
        net_put_be16(header + 10, net_checksum(header, 20));
    }
    pbuf_free(packet);
    if (allocated != count) {
        for (unsigned i = 0; i < allocated; i++)
            pbuf_free(fragments[i]);
        return -ENOBUFS;
    }
    int result = 0;
    for (unsigned i = 0; i < count; i++) {
        if (result < 0)
            pbuf_free(fragments[i]);
        else
            result = ethernet_output(interface, fragments[i], mac, 0x0800);
    }
    return result;
}
