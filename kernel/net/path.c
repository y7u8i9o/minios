/* Worker-owned path MTU cache and recent-transmission correlation. ICMP may
 * lower a path only when it quotes a recently queued packet exactly, including
 * its IP identification and first eight transport bytes. No quoted pointer is
 * retained, and route changes invalidate both tables. */
#include <net/ipv4.h>
#include <net/clock.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <lib/string.h>

#define PATHS 16
#define QUOTES 128
#define PATH_MS 600000
#define QUOTE_MS 30000
struct path {
    uint32_t destination;
    unsigned mtu;
    uint64_t expires;
};
struct transmission {
    uint8_t quote[28];
    uint64_t expires;
};
static struct path paths[PATHS];
static struct transmission transmissions[QUOTES];
static unsigned cursor;

void ipv4_path_flush(void)
{
    memset(paths, 0, sizeof paths);
    memset(transmissions, 0, sizeof transmissions);
}
unsigned ipv4_path_mtu(uint32_t destination, unsigned interface_mtu)
{
    for (unsigned i = 0; i < PATHS; i++) {
        if (paths[i].destination == destination && paths[i].expires > net_clock_ms())
            return MIN(interface_mtu, paths[i].mtu);
    }
    return interface_mtu;
}
void ipv4_path_lower(uint32_t destination, unsigned mtu)
{
    if (mtu < 68 || mtu > 65535)
        return;
    struct path *selected = &paths[0];
    uint64_t now = net_clock_ms();
    for (unsigned i = 0; i < PATHS; i++) {
        if (paths[i].destination == destination) {
            selected = &paths[i];
            break;
        }
        if (paths[i].expires < selected->expires)
            selected = &paths[i];
    }
    if (selected->destination == destination && selected->expires > now)
        mtu = MIN(mtu, selected->mtu);
    selected->destination = destination;
    selected->mtu = mtu;
    selected->expires = now + PATH_MS;
    tcp_path_changed(destination, mtu);
}
void ipv4_note_output(const struct pbuf *packet)
{
    if (packet->len < 28)
        return;
    struct transmission *entry = &transmissions[cursor++ % QUOTES];
    memcpy(entry->quote, packet->data, sizeof entry->quote);
    entry->expires = net_clock_ms() + QUOTE_MS;
}
bool ipv4_validate_quote(const uint8_t *quote, size_t length)
{
    if (length < 28 || quote[0] != 0x45 || net_checksum(quote, 20) ||
        net_get_be16(quote + 2) < 28 || !ipv4_local(net_get_be32(quote + 12)))
        return false;
    uint64_t now = net_clock_ms();
    for (unsigned i = 0; i < QUOTES; i++) {
        const struct transmission *entry = &transmissions[i];
        /* Routers change TTL and consequently the IPv4 checksum. Every other
         * quoted byte must match, including flags, lengths and transport data. */
        if (entry->expires > now && !memcmp(quote, entry->quote, 8) &&
            quote[9] == entry->quote[9] && !memcmp(quote + 12, entry->quote + 12, 16))
            return true;
    }
    return false;
}
void ipv4_path_feedback(const uint8_t *quote, size_t length, unsigned mtu)
{
    if (!ipv4_validate_quote(quote, length) || !(net_get_be16(quote + 6) & 0x4000)) {
        net_ip_stats.pmtu_rejected++;
        return;
    }
    unsigned total = net_get_be16(quote + 2);
    if (!mtu) {
        static const unsigned plateaus[] = {1492, 1280, 1006, 576, 296, 68};
        for (unsigned i = 0; i < sizeof plateaus / sizeof plateaus[0]; i++) {
            if (plateaus[i] < total) {
                mtu = plateaus[i];
                break;
            }
        }
    }
    if (mtu < 68 || mtu >= total) {
        net_ip_stats.pmtu_rejected++;
        return;
    }
    ipv4_path_lower(net_get_be32(quote + 16), mtu);
    net_ip_stats.pmtu_updates++;
}
