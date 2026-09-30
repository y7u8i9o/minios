/* TCP parsing and receive-side state transitions. Netd is the sole caller;
 * receive storage and endpoint notifications use tcp_lock for readers. */
#include "internal.h"
#include <net/byteorder.h>
#include <net/checksum.h>
#include <lib/string.h>
#include <errno.h>

static struct tcp_connection *lookup_connection(const struct tcp_segment *segment)
{
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (c->used && c->state != TCP_CLOSED && c->local_address == segment->destination &&
            c->peer_address == segment->source && c->local_port == segment->destination_port &&
            c->peer_port == segment->source_port)
            return c;
    }
    return NULL;
}

static struct tcp_endpoint *lookup_listener(const struct tcp_segment *segment)
{
    struct tcp_endpoint *listener = NULL;
    spin_lock(&tcp_lock);
    for (unsigned i = 0; i < TCP_ENDPOINTS; i++) {
        struct tcp_endpoint *e = &tcp_endpoints[i];
        if (e->socket && e->listening && e->local_port == segment->destination_port &&
            (!e->local_address || e->local_address == segment->destination)) {
            listener = e;
            break;
        }
    }
    spin_unlock(&tcp_lock);
    return listener;
}

static void passive_open(struct tcp_endpoint *listener, const struct tcp_segment *segment)
{
    if (!tcp_random_ready())
        return;
    unsigned incomplete, ready;
    tcp_listener_counts(listener, &incomplete, &ready);
    if (incomplete >= MIN(listener->backlog, TCP_SYN_BACKLOG)) {
        tcp_counters.backlog_drops++;
        return;
    }
    struct net_route route;
    if (net_route_lookup(segment->source, &route) < 0)
        return;
    struct tcp_connection *c = tcp_connection_alloc();
    if (!c) {
        tcp_counters.backlog_drops++;
        return;
    }
    c->listener = listener;
    c->state = TCP_SYN_RECEIVED;
    c->local_address = segment->destination;
    c->peer_address = segment->source;
    c->local_port = segment->destination_port;
    c->peer_port = segment->source_port;
    c->iss = tcp_initial_sequence();
    c->irs = segment->sequence;
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->rcv_nxt = segment->sequence + 1;
    c->peer_window = segment->window;
    c->snd_wl1 = segment->sequence;
    c->snd_wl2 = 0;
    c->local_mss = MIN(TCP_LOCAL_MSS, route.netif->mtu - 40);
    c->peer_mss = MIN(segment->mss, c->local_mss);
    /* The timestamp clock offset is drawn like an initial sequence. */
    c->ts_offset = tcp_initial_sequence();
    tcp_negotiate(c, segment);
    tcp_counters.passive_opens++;
    tcp_emit(c, TCP_SYN | TCP_ACK, c->iss, NULL, 0);
    tcp_start_control_timer(c);
}

static void active_reply(struct tcp_connection *c, const struct tcp_segment *segment)
{
    bool acknowledged = segment->flags & TCP_ACK;
    bool valid_ack = acknowledged && segment->acknowledgement == c->snd_nxt;
    if (acknowledged && !valid_ack) {
        tcp_reset_reply(segment);
        return;
    }
    if (segment->flags & TCP_RST) {
        if (valid_ack)
            tcp_fail(c, -ECONNREFUSED);
        return;
    }
    if (!(segment->flags & TCP_SYN))
        return;
    c->irs = segment->sequence;
    c->rcv_nxt = segment->sequence + 1;
    c->peer_window = segment->window;
    c->peer_mss = MIN(segment->mss, c->local_mss);
    c->snd_wl1 = segment->sequence;
    c->snd_wl2 = segment->acknowledgement;
    tcp_negotiate(c, segment);
    if (valid_ack) {
        tcp_established(c);
        tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0);
    } else {
        /* Simultaneous active open: retain our original SYN sequence. */
        c->state = TCP_SYN_RECEIVED;
        tcp_emit(c, TCP_SYN | TCP_ACK, c->iss, NULL, 0);
        tcp_start_control_timer(c);
        tcp_publish(c);
    }
}

static bool acceptable_sequence(struct tcp_connection *c, const struct tcp_segment *segment)
{
    unsigned window = tcp_receive_window(c);
    uint32_t sequence = segment->sequence;
    size_t length = segment->length + !!(segment->flags & TCP_SYN) + !!(segment->flags & TCP_FIN);
    /* RFC 9293 accepts nothing with length in a closed window. A FIN
     * without data at RCV.NXT needs no receive space, so it is accepted;
     * otherwise a peer whose FIN meets a full window waits for its own
     * retransmission timeout after the reader has emptied the store. */
    bool bare_fin = !segment->length && (segment->flags & (TCP_SYN | TCP_FIN)) == TCP_FIN;
    if (!window)
        return (!length || bare_fin) && sequence == c->rcv_nxt;
    if (!length)
        return !tcp_before(sequence, c->rcv_nxt) && tcp_before(sequence, c->rcv_nxt + window);
    uint32_t last = sequence + (uint32_t)length - 1;
    return (!tcp_before(sequence, c->rcv_nxt) && tcp_before(sequence, c->rcv_nxt + window)) ||
           (!tcp_before(last, c->rcv_nxt) && tcp_before(last, c->rcv_nxt + window));
}

/* ACK processing may finish LAST_ACK and release an orphan; false means the
 * caller must no longer use c. Sequence comparisons cover 32-bit wrap. */
static bool acknowledge(struct tcp_connection *c, const struct tcp_segment *segment)
{
    uint32_t ack = segment->acknowledgement;
    if (tcp_after(ack, c->snd_nxt)) {
        tcp_challenge_ack(c);
        return false;
    }
    tcp_data_ack(c, segment);
    if (!tcp_before(ack, c->snd_una)) {
        if (tcp_after(segment->sequence, c->snd_wl1) ||
            (segment->sequence == c->snd_wl1 && !tcp_before(ack, c->snd_wl2))) {
            c->peer_window = tcp_segment_window(c, segment);
            c->snd_wl1 = segment->sequence;
            c->snd_wl2 = ack;
        }
    }

    if (c->fin_sent && tcp_after(ack, c->fin_sequence)) {
        c->control_deadline = 0;
        switch (c->state) {
        case TCP_FIN_WAIT_1:
            c->state = TCP_FIN_WAIT_2;
            c->lifetime_deadline = net_clock_ms() + TCP_ORPHAN_MS;
            break;
        case TCP_CLOSING:
            tcp_enter_time_wait(c);
            break;
        case TCP_LAST_ACK:
            tcp_fail(c, 0);
            return false;
        default:
            break;
        }
    }
    return true;
}

/* This is step R1 of RFC 7323 section 5.3, which runs before the sequence
 * check. With timestamps negotiated, a segment without one is dropped
 * silently, and a segment whose timestamp is older than TS.Recent is a
 * duplicate from an earlier incarnation of the sequence space, so it is
 * dropped and answered with a rate-limited ACK. A reset is exempt from both checks. After 24
 * idle days TS.Recent is no longer trusted and the segment is accepted. */
static bool timestamp_acceptable(struct tcp_connection *c, const struct tcp_segment *segment)
{
    if (!c->timestamps || (segment->flags & TCP_RST))
        return true;
    if (!segment->has_timestamp) {
        tcp_counters.timestamp_missing++;
        return false;
    }
    if (tcp_before(segment->timestamp_value, c->ts_recent) &&
        net_clock_ms() - c->ts_recent_age <= TCP_PAWS_IDLE_MS) {
        tcp_counters.paws_rejected++;
        tcp_challenge_ack(c);
        return false;
    }
    return true;
}

/* By RFC 7323 section 4.3, TS.Recent follows the newest timestamp of a
 * segment that begins at or before the last acknowledgement sent, which
 * keeps the timestamp of the earliest unacknowledged segment when ACKs are
 * delayed or segments arrive out of order. */
static void timestamp_update(struct tcp_connection *c, const struct tcp_segment *segment)
{
    if (!c->timestamps || !segment->has_timestamp ||
        tcp_after(segment->sequence, c->last_ack_sent))
        return;
    if (!tcp_before(segment->timestamp_value, c->ts_recent) ||
        net_clock_ms() - c->ts_recent_age > TCP_PAWS_IDLE_MS) {
        c->ts_recent = segment->timestamp_value;
        c->ts_recent_age = net_clock_ms();
    }
}

static void synchronized_input(struct tcp_connection *c, const struct tcp_segment *segment)
{
    if (!timestamp_acceptable(c, segment))
        return;
    /* Duplicate handshake replies recover a lost final ACK. Duplicate FIN
     * restarts TIME_WAIT; RST cannot assassinate that retained reservation. */
    if (c->state == TCP_TIME_WAIT) {
        if (segment->flags & TCP_RST)
            return;
        if ((segment->flags & TCP_FIN) && segment->sequence + segment->length + 1 == c->rcv_nxt) {
            tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0);
            tcp_enter_time_wait(c);
        }
        return;
    }
    if ((segment->flags & TCP_SYN) && segment->sequence == c->irs) {
        if (c->state == TCP_SYN_RECEIVED && !(segment->flags & (TCP_ACK | TCP_RST))) {
            tcp_emit(c, TCP_SYN | TCP_ACK, c->iss, NULL, 0);
            return;
        }
        if ((segment->flags & (TCP_SYN | TCP_ACK | TCP_RST)) == (TCP_SYN | TCP_ACK) &&
            segment->acknowledgement == c->iss + 1) {
            if (c->state == TCP_SYN_RECEIVED && !c->listener)
                tcp_established(c);
            tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0);
            return;
        }
    }
    if (!acceptable_sequence(c, segment)) {
        if (!(segment->flags & TCP_RST))
            tcp_challenge_ack(c);
        return;
    }
    timestamp_update(c, segment);
    if (segment->flags & TCP_RST) {
        if (segment->sequence == c->rcv_nxt) {
            tcp_counters.resets++;
            tcp_fail(c, -ECONNRESET);
        } else {
            tcp_challenge_ack(c);
        }
        return;
    }
    if (segment->flags & TCP_SYN) {
        tcp_challenge_ack(c);
        return;
    }
    if (!(segment->flags & TCP_ACK))
        return;
    if (c->state == TCP_SYN_RECEIVED) {
        if (segment->acknowledgement != c->snd_nxt || segment->sequence != c->rcv_nxt) {
            tcp_reset_reply(segment);
            return;
        }
        tcp_established(c);
        if (!c->used)
            return;
    }
    if (segment->length && !c->endpoint && !c->listener) {
        /* A final file close has removed every possible reader. Reset on
         * later data instead of acknowledging bytes nobody can receive. */
        tcp_emit(c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0);
        tcp_fail(c, -ECONNRESET);
        return;
    }
    if (!acknowledge(c, segment))
        return;
    tcp_receive_segment(c, segment);
    tcp_flush(c);
    tcp_maybe_fin(c);
    tcp_schedule(c);
    tcp_publish(c);
}

void tcp_input(struct netif *interface, struct pbuf *packet)
{
    struct tcp_segment segment;
    if (!tcp_parse_segment(packet->data, packet->len, &segment)) {
        tcp_counters.invalid++;
        pbuf_free(packet);
        return;
    }
    struct tcp_connection *c = lookup_connection(&segment);
    if (c) {
        if (c->state == TCP_SYN_SENT)
            active_reply(c, &segment);
        else
            synchronized_input(c, &segment);
    } else {
        struct tcp_endpoint *listener = lookup_listener(&segment);
        if (listener && (segment.flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN)
            passive_open(listener, &segment);
        else
            tcp_reset_reply(&segment);
    }
    pbuf_free(packet);
}

void tcp_icmp_error(const uint8_t *ip, size_t length, int error)
{
    if (length < 28 || ip[0] != 0x45 || ip[9] != IPPROTO_TCP || net_checksum(ip, 20))
        return;
    uint32_t local = net_get_be32(ip + 12);
    uint32_t peer = net_get_be32(ip + 16);
    uint16_t local_port = net_get_be16(ip + 20);
    uint16_t peer_port = net_get_be16(ip + 22);
    uint32_t sequence = net_get_be32(ip + 24);
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (!c->used || c->state != TCP_SYN_SENT || c->local_address != local ||
            c->peer_address != peer || c->local_port != local_port || c->peer_port != peer_port ||
            sequence != c->iss)
            continue;
        /* Only a handshake is aborted by an ICMP error; icmp_input has
         * already matched the quote against a recent transmission. An
         * established connection keeps retransmitting until its own
         * deadlines fail it, and fragmentation-needed errors reach it
         * through the path MTU cache instead. */
        tcp_fail(c, -error);
    }
}
