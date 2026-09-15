/* Bounded send storage, ACK accounting, RTT estimation and conservative
 * Tahoe congestion control. Every entry point runs on netd. */
#include "internal.h"
#include <lib/string.h>
#include <errno.h>

void tcp_transfer_init(struct tcp_connection *c)
{
    struct net_route route;
    if (net_route_lookup(c->peer_address, &route) == 0)
        c->peer_mss = MIN(c->peer_mss, ipv4_path_mtu(c->peer_address, route.netif->mtu) - 40);
    c->congestion_window = c->peer_mss;
    c->slow_start_threshold = 65535;
    c->rto_ms = 1000;
}

static void loss_window(struct tcp_connection *c)
{
    c->slow_start_threshold = MAX(c->transmit_sent / 2, 2u * c->peer_mss);
    c->congestion_window = c->peer_mss;
    c->congestion_credit = 0;
    c->sampling = false; /* Karn: ACKs after retransmission are ambiguous. */
    c->recovering = true;
    c->recovery_end = c->snd_nxt;
}

static void retransmit(struct tcp_connection *c)
{
    size_t length = MIN(c->transmit_sent, c->peer_mss);
    if (length) {
        tcp_emit(c, TCP_ACK, c->snd_una, c->transmit, length);
        tcp_counters.retransmits++;
    }
    c->sampling = false;
    c->data_deadline = net_clock_ms() + c->rto_ms;
}

void tcp_flush(struct tcp_connection *c)
{
    if (!c->transmit_length || c->fin_sent ||
        (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
        return;
    uint64_t now = net_clock_ms();
    if (!c->transmit_sent && c->last_transmit && now - c->last_transmit >= c->rto_ms)
        c->congestion_window = MIN(c->congestion_window, c->peer_mss);
    unsigned limit = MIN(c->congestion_window, c->peer_window);
    for (unsigned batch = 0;
         batch < 8 && c->transmit_sent < c->transmit_length && c->transmit_sent < limit;
         batch++) {
        size_t length = MIN(c->peer_mss, c->transmit_length - c->transmit_sent);
        length = MIN(length, limit - c->transmit_sent);
        int result =
            tcp_emit(c, TCP_ACK | TCP_PSH, c->snd_nxt, c->transmit + c->transmit_sent, length);
        if (result < 0)
            break; /* Retain bytes and retry after bounded local pressure. */
        if (!c->sampling && !c->transmit_sent && !c->recovering) {
            c->sampling = true;
            c->sample_time = now;
            c->sample_end = c->snd_nxt + length;
        }
        c->snd_nxt += length;
        c->transmit_sent += length;
        c->last_transmit = now;
    }
    if (!c->data_deadline)
        c->data_deadline = now + c->rto_ms;
}

static void sample_rtt(struct tcp_connection *c, uint32_t acknowledgement)
{
    if (!c->sampling || tcp_before(acknowledgement, c->sample_end))
        return;
    uint32_t sample = MAX(1u, MIN(net_clock_ms() - c->sample_time, 60000u));
    if (!c->srtt_ms) {
        c->srtt_ms = sample;
        c->rtt_variance_ms = MAX(sample / 2, 1u);
    } else {
        uint32_t delta = c->srtt_ms > sample ? c->srtt_ms - sample : sample - c->srtt_ms;
        c->rtt_variance_ms = (3 * c->rtt_variance_ms + delta) / 4;
        c->srtt_ms = (7 * c->srtt_ms + sample) / 8;
    }
    c->rto_ms = MIN(60000u, MAX(1000u, c->srtt_ms + MAX(1u, 4 * c->rtt_variance_ms)));
    c->sampling = false;
}

void tcp_data_ack(struct tcp_connection *c, const struct tcp_segment *segment)
{
    uint32_t ack = segment->acknowledgement;
    if (tcp_before(ack, c->snd_una) || tcp_after(ack, c->snd_nxt))
        return;
    if (ack == c->snd_una) {
        if (c->transmit_sent && !segment->length && !(segment->flags & (TCP_SYN | TCP_FIN)) &&
            segment->window == c->peer_window && ++c->duplicate_acks == 3 && !c->recovering) {
            loss_window(c);
            retransmit(c);
        }
        return;
    }
    size_t consumed = MIN((uint32_t)(ack - c->snd_una), c->transmit_sent);
    sample_rtt(c, ack);
    if (consumed) {
        memmove(c->transmit, c->transmit + consumed, c->transmit_length - consumed);
        c->transmit_length -= consumed;
        c->transmit_sent -= consumed;
        if (c->congestion_window < c->slow_start_threshold) {
            c->congestion_window += MIN(consumed, c->peer_mss);
        } else {
            c->congestion_credit += consumed;
            if (c->congestion_credit >= c->congestion_window) {
                c->congestion_credit -= c->congestion_window;
                c->congestion_window += c->peer_mss;
            }
        }
        c->congestion_window = MIN(c->congestion_window, TCP_SEND_CAPACITY);
    }
    c->snd_una = ack;
    c->duplicate_acks = 0;
    c->data_retries = 0;
    if (c->recovering && !tcp_before(ack, c->recovery_end))
        c->recovering = false;
    uint64_t now = net_clock_ms();
    c->data_deadline = c->transmit_length ? now + c->rto_ms : 0;
    c->progress_deadline = c->transmit_length ? now + TCP_PROGRESS_MS : 0;
}

void tcp_data_timeout(struct tcp_connection *c)
{
    uint64_t now = net_clock_ms();
    if (now >= c->progress_deadline || c->data_retries == TCP_DATA_RETRIES) {
        tcp_counters.timeouts++;
        tcp_emit(c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0);
        tcp_fail(c, -ETIMEDOUT);
        return;
    }
    c->data_retries++;
    if (c->peer_window && c->transmit_sent) {
        /* Bound recovery when ICMP is filtered. Smaller segments test the
         * path conservatively, while the original progress deadline remains. */
        unsigned fallback = c->data_retries >= 6 ? 68 : c->data_retries >= 4 ? 296 : 576;
        if (c->data_retries >= 2 && (unsigned)c->peer_mss + 40 > fallback)
            ipv4_path_lower(c->peer_address, fallback);
    }
    if (!c->peer_window) {
        /* An old sequence byte cannot enter the peer stream. Its ACK reports
         * the current window even when the window-opening ACK was lost. */
        tcp_emit(c, TCP_ACK, c->snd_una - 1, c->transmit, 1);
        c->sampling = false;
    } else if (c->transmit_sent) {
        loss_window(c);
        retransmit(c);
    } else {
        tcp_flush(c);
    }
    c->rto_ms = MIN(c->rto_ms * 2, 60000u);
    c->data_deadline = MIN(now + c->rto_ms, c->progress_deadline);
}

void tcp_path_changed(uint32_t destination, unsigned mtu)
{
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (!c->used || c->peer_address != destination || c->peer_mss <= mtu - 40)
            continue;
        c->peer_mss = mtu - 40;
        c->congestion_window = MIN(c->congestion_window, c->peer_mss);
        c->sampling = false;
        if (c->transmit_length) {
            c->data_deadline = net_clock_ms();
            tcp_schedule(c);
        }
    }
}

void tcp_interface_changed(struct netif *interface, int error)
{
    uint32_t local = ipv4_address(interface);
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (c->used && c->local_address == local)
            tcp_fail(c, -error);
    }
}
