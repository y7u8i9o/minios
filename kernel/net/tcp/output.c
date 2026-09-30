/* Packet construction uses fresh buffers. Device completion never owns
 * connection state or acknowledges TCP sequence space. */
#include "internal.h"
#include <net/byteorder.h>
#include <net/checksum.h>
#include <lib/string.h>
#include <errno.h>

/* Options of one outgoing segment. The layouts follow the examples of
 * RFC 7323 appendix A and RFC 2018: four-byte aligned, padded with NOP. */
struct tcp_out_options {
    uint16_t mss;         /* nonzero: MSS, SYN only */
    int window_scale;     /* nonnegative: window scale shift, SYN only */
    bool timestamp;
    uint32_t ts_value, ts_echo;
};

static size_t options_length(const struct tcp_out_options *o)
{
    return (o->mss ? 4 : 0) + (o->window_scale >= 0 ? 4 : 0) +
           (o->timestamp ? TCP_TIMESTAMP_SPACE : 0);
}

static void put_options(uint8_t *p, const struct tcp_out_options *o)
{
    if (o->mss) {
        p[0] = 2;
        p[1] = 4;
        net_put_be16(p + 2, o->mss);
        p += 4;
    }
    if (o->timestamp) {
        p[0] = 1;
        p[1] = 1;
        p[2] = 8;
        p[3] = 10;
        net_put_be32(p + 4, o->ts_value);
        net_put_be32(p + 8, o->ts_echo);
        p += TCP_TIMESTAMP_SPACE;
    }
    if (o->window_scale >= 0) {
        p[0] = 1;
        p[1] = 3;
        p[2] = 3;
        p[3] = (uint8_t)o->window_scale;
    }
}

static int emit_segment(uint32_t source,
                        uint32_t destination,
                        uint16_t source_port,
                        uint16_t destination_port,
                        uint32_t sequence,
                        uint32_t acknowledgement,
                        uint16_t window,
                        const struct tcp_out_options *options,
                        uint8_t flags,
                        const void *data,
                        size_t length)
{
    size_t header_length = 20 + options_length(options);
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
    put_options(header + 20, options);
    if (length)
        memcpy(header + header_length, data, length);
    net_put_be16(header + 16, tcp_checksum(source, destination, header, packet->len));
    return ipv4_output(packet, source, destination, IPPROTO_TCP);
}

/* A SYN offers window scaling and timestamps in SYN_SENT and repeats the
 * negotiated subset in a SYN ACK. Every later segment of a connection that
 * negotiated timestamps carries one; RFC 7323 section 3.2 requires it on
 * every segment but a reset and recommends it there too. The window is
 * shifted by rcv_scale unless the segment is a SYN, whose window RFC 7323
 * leaves unscaled. */
int tcp_emit(
    struct tcp_connection *c, uint8_t flags, uint32_t sequence, const void *data, size_t length)
{
    bool syn = flags & TCP_SYN;
    bool offer = c->state == TCP_SYN_SENT;
    struct tcp_out_options options = {.window_scale = -1};
    if (syn) {
        options.mss = c->local_mss;
        if (offer || c->window_scaling)
            options.window_scale = offer ? TCP_WINDOW_SHIFT : c->rcv_scale;
    }
    if ((syn && offer) || c->timestamps) {
        options.timestamp = true;
        options.ts_value = tcp_timestamp_now(c);
        options.ts_echo = flags & TCP_ACK ? c->ts_recent : 0;
    }
    unsigned window = tcp_receive_window(c);
    if (!syn && c->window_scaling)
        window >>= c->rcv_scale;
    if (flags & TCP_ACK)
        c->last_ack_sent = c->rcv_nxt;
    return emit_segment(c->local_address,
                        c->peer_address,
                        c->local_port,
                        c->peer_port,
                        sequence,
                        flags & TCP_ACK ? c->rcv_nxt : 0,
                        (uint16_t)MIN(window, 65535u),
                        &options,
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
    struct tcp_out_options none = {.window_scale = -1};
    emit_segment(segment->destination,
                 segment->source,
                 segment->destination_port,
                 segment->source_port,
                 sequence,
                 acknowledgement,
                 0,
                 &none,
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
