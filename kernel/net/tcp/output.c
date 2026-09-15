/* Packet construction uses fresh buffers. Device completion never owns
 * connection state or acknowledges TCP sequence space. */
#include "internal.h"
#include <net/byteorder.h>
#include <net/checksum.h>
#include <lib/string.h>
#include <errno.h>

static int emit_segment(uint32_t source,
                        uint32_t destination,
                        uint16_t source_port,
                        uint16_t destination_port,
                        uint32_t sequence,
                        uint32_t acknowledgement,
                        uint16_t window,
                        uint16_t mss,
                        uint8_t flags,
                        const void *data,
                        size_t length)
{
    size_t header_length = flags & TCP_SYN ? 24 : 20;
    struct pbuf *packet = pbuf_alloc(length ? PBUF_DATA : PBUF_CONTROL);
    if (!packet)
        return -ENOBUFS;
    uint8_t *header = pbuf_put(packet, header_length + length);
    if (!header) {
        pbuf_free(packet);
        return -EMSGSIZE;
    }
    memset(header, 0, header_length);
    net_put_be16(header, source_port);
    net_put_be16(header + 2, destination_port);
    net_put_be32(header + 4, sequence);
    net_put_be32(header + 8, acknowledgement);
    header[12] = (uint8_t)(header_length / 4) << 4;
    header[13] = flags;
    net_put_be16(header + 14, window);
    if (flags & TCP_SYN) {
        header[20] = 2;
        header[21] = 4;
        net_put_be16(header + 22, mss);
    }
    if (length)
        memcpy(header + header_length, data, length);
    net_put_be16(header + 16, tcp_checksum(source, destination, header, packet->len));
    return ipv4_output(packet, source, destination, IPPROTO_TCP);
}

int tcp_emit(
    struct tcp_connection *c, uint8_t flags, uint32_t sequence, const void *data, size_t length)
{
    return emit_segment(c->local_address,
                        c->peer_address,
                        c->local_port,
                        c->peer_port,
                        sequence,
                        flags & TCP_ACK ? c->rcv_nxt : 0,
                        (uint16_t)tcp_receive_window(c),
                        c->local_mss,
                        flags,
                        data,
                        length);
}

static bool reply_allowed(void)
{
    static uint64_t window;
    static unsigned count;
    uint64_t now = net_clock_ms();
    if (now < window || now - window >= 1000) {
        window = now;
        count = 0;
    }
    if (count == 20) {
        tcp_counters.suppressed++;
        return false;
    }
    count++;
    return true;
}

void tcp_reset_reply(const struct tcp_segment *segment)
{
    if ((segment->flags & TCP_RST) || !reply_allowed())
        return;
    uint32_t sequence = 0;
    uint32_t acknowledgement = 0;
    uint8_t flags = TCP_RST;
    if (segment->flags & TCP_ACK) {
        sequence = segment->acknowledgement;
    } else {
        flags |= TCP_ACK;
        acknowledgement = segment->sequence + (uint32_t)segment->length;
        acknowledgement += !!(segment->flags & TCP_SYN) + !!(segment->flags & TCP_FIN);
    }
    emit_segment(segment->destination,
                 segment->source,
                 segment->destination_port,
                 segment->source_port,
                 sequence,
                 acknowledgement,
                 0,
                 0,
                 flags,
                 NULL,
                 0);
    tcp_counters.resets++;
}

void tcp_challenge_ack(struct tcp_connection *connection)
{
    if (reply_allowed())
        tcp_emit(connection, TCP_ACK, connection->snd_nxt, NULL, 0);
}
