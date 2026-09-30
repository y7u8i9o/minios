/* Bounded send storage, ACK accounting, RTT estimation from a timed
 * segment or from echoed timestamps, conservative Tahoe congestion control,
 * and SACK-based loss recovery (RFC 6675) with a bounded scoreboard for
 * connections that negotiated SACK. Every entry point runs on netd. */
#include "internal.h"
#include <lib/string.h>
#include <errno.h>

void tcp_transfer_init(struct tcp_connection *c)
{
    struct net_route route;
    if (net_route_lookup(c->peer_address, &route) == 0)
        c->peer_mss = MIN(c->peer_mss, ipv4_path_mtu(c->peer_address, route.netif->mtu) - 40);
    c->congestion_window = tcp_send_mss(c);
    c->slow_start_threshold = TCP_SEND_CAPACITY;
    c->rto_ms = 1000;
}

static void loss_window(struct tcp_connection *c)
{
    c->slow_start_threshold = MAX(c->transmit_sent / 2, 2u * tcp_send_mss(c));
    c->congestion_window = tcp_send_mss(c);
    c->congestion_credit = 0;
    c->sampling = false; /* Karn: ACKs after retransmission are ambiguous. */
    c->recovering = true;
    c->recovery_end = c->snd_nxt;
}

static void retransmit(struct tcp_connection *c)
{
    size_t length = MIN(c->transmit_sent, tcp_send_mss(c));
    if (length) {
        tcp_emit(c, TCP_ACK, c->snd_una, c->transmit, length);
        tcp_counters.retransmits++;
    }
    c->high_rxt = c->snd_una + (uint32_t)length;
    c->sampling = false;
    c->data_deadline = net_clock_ms() + c->rto_ms;
}

/* The scoreboard (RFC 6675 section 3) holds at most TCP_SCOREBOARD sorted,
 * disjoint, non-adjacent ranges of SACKed sequence space inside
 * [snd_una, snd_nxt]. When a new block would need one range more, the
 * highest range is dropped: forgetting that the peer holds data is safe,
 * since at worst that data is sent again, while the ranges nearest to
 * snd_una decide what is retransmitted next. */
static void scoreboard_trim(struct tcp_connection *c)
{
    unsigned kept = 0;
    for (unsigned i = 0; i < c->scoreboard_count; i++) {
        struct tcp_range r = c->scoreboard[i];
        if (!tcp_after(r.end, c->snd_una))
            continue;
        if (tcp_before(r.start, c->snd_una))
            r.start = c->snd_una;
        c->scoreboard[kept++] = r;
    }
    c->scoreboard_count = kept;
}

/* Merges [start, end) into the scoreboard; returns the bytes it adds. */
static uint32_t scoreboard_insert(struct tcp_connection *c, uint32_t start, uint32_t end)
{
    uint32_t added = end - start;
    struct tcp_range merged = {start, end};
    struct tcp_range kept[TCP_SCOREBOARD + 1];
    unsigned count = 0, position = 0;
    for (unsigned i = 0; i < c->scoreboard_count; i++) {
        struct tcp_range r = c->scoreboard[i];
        if (tcp_before(r.end, start)) {
            kept[count++] = r;
            position = count;
            continue;
        }
        if (tcp_after(r.start, end)) {
            kept[count++] = r;
            continue;
        }
        /* Overlapping or adjacent: count the overlap once and absorb it. */
        uint32_t low = tcp_after(r.start, start) ? r.start : start;
        uint32_t high = tcp_before(r.end, end) ? r.end : end;
        if (tcp_before(low, high))
            added -= high - low;
        if (tcp_before(r.start, merged.start))
            merged.start = r.start;
        if (tcp_after(r.end, merged.end))
            merged.end = r.end;
    }
    memmove(kept + position + 1, kept + position, (count - position) * sizeof kept[0]);
    kept[position] = merged;
    count++;
    if (count > TCP_SCOREBOARD) {
        count = TCP_SCOREBOARD;
        tcp_counters.scoreboard_drops++;
    }
    memcpy(c->scoreboard, kept, count * sizeof kept[0]);
    c->scoreboard_count = count;
    return added;
}

/* Accepts the SACK blocks of an ACK that lie within [snd_una, snd_nxt]; a
 * block below snd_una reports a duplicate and is ignored. Returns the
 * number of bytes SACKed for the first time. */
static uint32_t scoreboard_update(struct tcp_connection *c, const struct tcp_segment *segment)
{
    uint32_t added = 0;
    for (unsigned i = 0; i < segment->sack_count; i++) {
        uint32_t start = segment->sack[i][0], end = segment->sack[i][1];
        if (!tcp_before(start, end) || tcp_before(start, c->snd_una) ||
            tcp_after(end, c->snd_nxt))
            continue;
        tcp_counters.sack_blocks_received++;
        added += scoreboard_insert(c, start, end);
    }
    return added;
}

/* The unSACKed holes of [snd_una, snd_nxt) are the gaps before each range
 * and after the last one. Every byte of one gap has the same ranges above
 * it, so IsLost (RFC 6675 section 4) is decided per gap: a byte is lost
 * when TCP_DUP_THRESHOLD ranges, or more than TCP_DUP_THRESHOLD - 1
 * segments of SACKed bytes, lie above it. In a recovery started by a
 * timeout, every unSACKed byte below recovery_end counts as lost. */
struct sack_gap {
    uint32_t start, end; /* the hole */
    uint32_t lost_end;   /* [start, lost_end) is lost */
    bool below_sacked;   /* a SACKed range lies above the hole */
};

static unsigned sack_gaps(struct tcp_connection *c, struct sack_gap *gaps)
{
    uint32_t mss = tcp_send_mss(c);
    uint32_t above = 0;
    for (unsigned i = 0; i < c->scoreboard_count; i++)
        above += c->scoreboard[i].end - c->scoreboard[i].start;
    unsigned count = 0;
    uint32_t cursor = c->snd_una;
    for (unsigned i = 0; i <= c->scoreboard_count; i++) {
        uint32_t end = i < c->scoreboard_count ? c->scoreboard[i].start : c->snd_nxt;
        unsigned ranges_above = c->scoreboard_count - i;
        if (tcp_before(cursor, end)) {
            struct sack_gap *g = &gaps[count++];
            g->start = cursor;
            g->end = end;
            g->below_sacked = ranges_above > 0;
            if (c->rto_recovery) {
                uint32_t limit = c->recovery_end;
                g->lost_end = !tcp_before(limit, end) ? end : tcp_after(limit, cursor) ? limit : cursor;
            } else if (ranges_above >= TCP_DUP_THRESHOLD ||
                       above > (TCP_DUP_THRESHOLD - 1) * mss) {
                g->lost_end = end;
            } else {
                g->lost_end = cursor;
            }
        }
        if (i < c->scoreboard_count) {
            above -= c->scoreboard[i].end - c->scoreboard[i].start;
            cursor = c->scoreboard[i].end;
        }
    }
    return count;
}

/* SetPipe (RFC 6675 section 4): the bytes in flight are the unSACKed
 * bytes not considered lost plus the retransmitted ones below high_rxt. */
static uint32_t sack_pipe(struct tcp_connection *c, const struct sack_gap *gaps, unsigned count)
{
    uint32_t pipe = 0;
    for (unsigned i = 0; i < count; i++) {
        const struct sack_gap *g = &gaps[i];
        pipe += g->end - g->lost_end;
        if (tcp_after(c->high_rxt, g->start))
            pipe += (tcp_before(c->high_rxt, g->end) ? c->high_rxt : g->end) - g->start;
    }
    return pipe;
}

static void sack_retransmit(struct tcp_connection *c, uint32_t sequence, uint32_t length)
{
    if (tcp_emit(c, TCP_ACK, sequence, c->transmit + (sequence - c->snd_una), length) < 0)
        return;
    c->high_rxt = sequence + length;
    c->sampling = false;
    tcp_counters.retransmits++;
    tcp_counters.sack_retransmits++;
}

static bool send_new_segment(struct tcp_connection *c)
{
    size_t limit = MIN(c->transmit_length, (size_t)c->peer_window);
    if (c->transmit_sent >= limit)
        return false;
    size_t length = MIN(tcp_send_mss(c), limit - c->transmit_sent);
    if (tcp_emit(c, TCP_ACK | TCP_PSH, c->snd_nxt, c->transmit + c->transmit_sent, length) < 0)
        return false;
    c->snd_nxt += length;
    c->transmit_sent += length;
    c->last_transmit = net_clock_ms();
    return true;
}

/* The transmission loop of RFC 6675 section 5, step (C): while the
 * congestion window exceeds the pipe by a full segment, send what NextSeg
 * selects. Rule (1) is the first lost byte above high_rxt, rule (2) new
 * data within the peer's window, and rule (3), during fast recovery only,
 * the first unSACKed byte above high_rxt that lies below SACKed data. The
 * optional rescue retransmission of rule (4) is not implemented. At most
 * 16 segments leave per call, which bounds the work of one ACK on netd. */
static void sack_transmit(struct tcp_connection *c)
{
    uint32_t mss = tcp_send_mss(c);
    for (unsigned batch = 0; batch < 16; batch++) {
        struct sack_gap gaps[TCP_SCOREBOARD + 1];
        unsigned count = sack_gaps(c, gaps);
        if (sack_pipe(c, gaps, count) + mss > c->congestion_window)
            break;
        bool sent = false;
        for (unsigned i = 0; i < count && !sent; i++) {
            uint32_t first = tcp_after(c->high_rxt, gaps[i].start) ? c->high_rxt : gaps[i].start;
            if (tcp_before(first, gaps[i].lost_end)) {
                sack_retransmit(c, first, MIN(mss, gaps[i].lost_end - first));
                sent = true;
            }
        }
        if (!sent)
            sent = send_new_segment(c);
        for (unsigned i = 0; i < count && !sent && !c->rto_recovery; i++) {
            uint32_t first = tcp_after(c->high_rxt, gaps[i].start) ? c->high_rxt : gaps[i].start;
            if (gaps[i].below_sacked && tcp_before(first, gaps[i].end)) {
                sack_retransmit(c, first, MIN(mss, gaps[i].end - first));
                sent = true;
            }
        }
        if (!sent)
            break;
    }
    if (!c->data_deadline && c->transmit_sent)
        c->data_deadline = net_clock_ms() + c->rto_ms;
}

/* RFC 6675 section 5, step (4): the recovery point is the highest sequence
 * sent, the congestion window and the threshold become half the flight
 * (at least two segments), and the first segment presumed lost is sent at
 * once; the transmission loop then runs from tcp_flush. */
static void sack_enter_recovery(struct tcp_connection *c)
{
    uint32_t mss = tcp_send_mss(c);
    c->recovering = true;
    c->rto_recovery = false;
    c->recovery_end = c->snd_nxt;
    c->slow_start_threshold = MAX((uint32_t)c->transmit_sent / 2, 2 * mss);
    c->congestion_window = c->slow_start_threshold;
    c->congestion_credit = 0;
    c->sampling = false;
    c->high_rxt = c->snd_una;
    tcp_counters.sack_recoveries++;
    uint32_t end = c->scoreboard_count ? c->scoreboard[0].start : c->snd_nxt;
    sack_retransmit(c, c->snd_una, MIN(mss, end - c->snd_una));
}

/* RFC 6675 section 2: with SACK, an ACK that does not move snd_una counts
 * as a duplicate when it SACKs bytes not SACKed before. Recovery starts at
 * TCP_DUP_THRESHOLD duplicates, or earlier when the SACK rule already
 * declares the first unacknowledged byte lost. */
static void sack_duplicate(struct tcp_connection *c, const struct tcp_segment *segment)
{
    if (!scoreboard_update(c, segment) || !c->transmit_sent || segment->length ||
        (segment->flags & (TCP_SYN | TCP_FIN)))
        return;
    c->duplicate_acks++;
    if (c->recovering)
        return;
    struct sack_gap gaps[TCP_SCOREBOARD + 1];
    unsigned count = sack_gaps(c, gaps);
    bool first_lost = count && gaps[0].start == c->snd_una && gaps[0].lost_end != gaps[0].start;
    if (c->duplicate_acks >= TCP_DUP_THRESHOLD || first_lost)
        sack_enter_recovery(c);
}

void tcp_flush(struct tcp_connection *c)
{
    if (!c->transmit_length || c->fin_sent ||
        (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
        return;
    if (c->sack && c->recovering) {
        sack_transmit(c);
        return;
    }
    uint64_t now = net_clock_ms();
    if (!c->transmit_sent && c->last_transmit && now - c->last_transmit >= c->rto_ms)
        c->congestion_window = MIN(c->congestion_window, tcp_send_mss(c));
    unsigned limit = MIN(c->congestion_window, c->peer_window);
    for (unsigned batch = 0;
         batch < 8 && c->transmit_sent < c->transmit_length && c->transmit_sent < limit;
         batch++) {
        size_t length = MIN(tcp_send_mss(c), c->transmit_length - c->transmit_sent);
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

/* RFC 6298: one measurement in milliseconds, clamped to 1-60000 ms, updates
 * the smoothed estimate and the retransmission timeout. */
static void rtt_update(struct tcp_connection *c, uint32_t sample)
{
    sample = MAX(1u, MIN(sample, 60000u));
    if (!c->srtt_ms) {
        c->srtt_ms = sample;
        c->rtt_variance_ms = MAX(sample / 2, 1u);
    } else {
        uint32_t delta = c->srtt_ms > sample ? c->srtt_ms - sample : sample - c->srtt_ms;
        c->rtt_variance_ms = (3 * c->rtt_variance_ms + delta) / 4;
        c->srtt_ms = (7 * c->srtt_ms + sample) / 8;
    }
    c->rto_ms = MIN(60000u, MAX(1000u, c->srtt_ms + MAX(1u, 4 * c->rtt_variance_ms)));
}

/* Without timestamps one segment per flight is timed and Karn's rule
 * cancels the sample on any retransmission. */
static void sample_rtt(struct tcp_connection *c, uint32_t acknowledgement)
{
    if (!c->sampling || tcp_before(acknowledgement, c->sample_end))
        return;
    rtt_update(c, (uint32_t)(net_clock_ms() - c->sample_time));
    c->sampling = false;
}

/* With timestamps the echoed value identifies the transmission that the
 * peer acknowledged, retransmissions included, so Karn's rule is not
 * needed (RFC 7323 section 4). One sample is taken per flight, which keeps
 * the RFC 6298 gains meaningful: the first ACK that covers the data sent
 * when the previous sample was taken provides the next one. An echo of 0
 * or one that lies in the future is ignored. */
static void sample_timestamp(struct tcp_connection *c,
                             const struct tcp_segment *segment,
                             uint32_t acknowledgement)
{
    if (!segment->has_timestamp || !segment->timestamp_echo ||
        (c->ts_sample_valid && tcp_before(acknowledgement, c->ts_sample_end)))
        return;
    uint32_t elapsed = tcp_timestamp_now(c) - segment->timestamp_echo;
    if (elapsed > 60000)
        return;
    rtt_update(c, elapsed);
    tcp_counters.timestamp_samples++;
    c->ts_sample_valid = true;
    c->ts_sample_end = c->snd_nxt;
}

void tcp_data_ack(struct tcp_connection *c, const struct tcp_segment *segment)
{
    uint32_t ack = segment->acknowledgement;
    if (tcp_before(ack, c->snd_una) || tcp_after(ack, c->snd_nxt))
        return;
    if (ack == c->snd_una) {
        if (c->sack) {
            sack_duplicate(c, segment);
            return;
        }
        if (c->transmit_sent && !segment->length && !(segment->flags & (TCP_SYN | TCP_FIN)) &&
            tcp_segment_window(c, segment) == c->peer_window && ++c->duplicate_acks == 3 &&
            !c->recovering) {
            loss_window(c);
            retransmit(c);
        }
        return;
    }
    size_t consumed = MIN((uint32_t)(ack - c->snd_una), c->transmit_sent);
    if (c->timestamps)
        sample_timestamp(c, segment, ack);
    else
        sample_rtt(c, ack);
    if (consumed) {
        memmove(c->transmit, c->transmit + consumed, c->transmit_length - consumed);
        c->transmit_length -= consumed;
        c->transmit_sent -= consumed;
        /* RFC 6675 keeps the window during fast recovery and lets the pipe
         * decide what is sent; after a timeout slow start applies. */
        bool fast_recovery = c->sack && c->recovering && !c->rto_recovery;
        if (fast_recovery) {
            /* The window stays at the threshold set on entry. */
        } else if (c->congestion_window < c->slow_start_threshold) {
            c->congestion_window += MIN(consumed, tcp_send_mss(c));
        } else {
            c->congestion_credit += consumed;
            if (c->congestion_credit >= c->congestion_window) {
                c->congestion_credit -= c->congestion_window;
                c->congestion_window += tcp_send_mss(c);
            }
        }
        c->congestion_window = MIN(c->congestion_window, TCP_SEND_CAPACITY);
    }
    c->snd_una = ack;
    c->duplicate_acks = 0;
    c->data_retries = 0;
    if (c->sack) {
        scoreboard_trim(c);
        scoreboard_update(c, segment);
        if (tcp_before(c->high_rxt, ack))
            c->high_rxt = ack;
    }
    if (c->recovering && !tcp_before(ack, c->recovery_end)) {
        c->recovering = false;
        c->rto_recovery = false;
    }
    uint64_t now = net_clock_ms();
    c->data_deadline = c->transmit_length ? now + c->rto_ms : 0;
    c->progress_deadline = c->transmit_length ? now + TCP_PROGRESS_MS : 0;
    tcp_release_unused(c);
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
        /* RFC 6675 section 5.1 keeps using SACK information after a
         * timeout. A second consecutive timeout suggests that the peer
         * discarded data it had SACKed (RFC 2018 section 8), so the
         * scoreboard is cleared then. */
        if (c->data_retries > 1)
            c->scoreboard_count = 0;
        loss_window(c);
        c->rto_recovery = c->sack;
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
        c->congestion_window = MIN(c->congestion_window, tcp_send_mss(c));
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
