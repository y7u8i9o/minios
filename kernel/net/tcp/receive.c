/* One circular receive store serves both contiguous and out-of-order data.
 * Existing bytes win overlaps. Missing bytes can fill holes without copying
 * or delivering an already accepted byte twice. Since N14 the receiver
 * reports out-of-order ranges as SACK blocks and delays the ACK of
 * in-order data within the rules of RFC 1122 and RFC 5681. */
#include "internal.h"
#include <lib/string.h>

static bool present(struct tcp_connection *c, unsigned index)
{
    return c->receive_present[index / 8] & (1u << (index % 8));
}

void tcp_receive_discard(struct tcp_connection *c)
{
    /* Caller has acquired tcp_lock. */
    c->receive_count = 0;
    c->out_of_order = 0;
    c->pending_fin = false;
    c->sack_report_count = 0;
    if (c->receive_present)
        memset(c->receive_present, 0, TCP_RECEIVE_CAPACITY / 8);
}

/* stored_run finds, in the presence bitmap, the contiguous run of stored
 * out-of-order bytes that contains [start, end). RFC 2018 section 4
 * requires the first reported block to be exactly this run. The scan never
 * passes rcv_nxt (that byte is missing, or rcv_nxt would have advanced) or
 * the right edge, and it skips whole bitmap bytes where it can. The caller
 * has acquired tcp_lock. */
static struct tcp_range stored_run(struct tcp_connection *c, uint32_t start, uint32_t end,
                                   uint32_t right)
{
    unsigned base = c->receive_head + c->receive_count;
    while (tcp_after(start, c->rcv_nxt) &&
           present(c, (base + (uint32_t)(start - 1 - c->rcv_nxt)) % TCP_RECEIVE_CAPACITY))
        start--;
    while (tcp_before(end, right)) {
        unsigned index = (base + (uint32_t)(end - c->rcv_nxt)) % TCP_RECEIVE_CAPACITY;
        if (index % 8 == 0 && c->receive_present[index / 8] == 0xff &&
            tcp_before(end + 7, right)) {
            end += 8;
            continue;
        }
        if (!present(c, index))
            break;
        end++;
    }
    return (struct tcp_range){start, end};
}

/* By RFC 2018 section 4, the first block reported is the run containing the
 * most recently received out-of-order segment, followed by the blocks
 * reported most recently. The new run absorbs every reported block it
 * overlaps or touches; blocks at or below rcv_nxt are dropped. At most
 * TCP_SACK_REPORT blocks are retained. */
static void sack_report(struct tcp_connection *c, uint32_t start, uint32_t end)
{
    struct tcp_range merged = {start, end};
    struct tcp_range retained[TCP_SACK_REPORT];
    unsigned count = 0;
    for (unsigned i = 0; i < c->sack_report_count; i++) {
        struct tcp_range r = c->sack_report[i];
        if (!tcp_after(r.end, c->rcv_nxt))
            continue;
        if (tcp_before(r.start, c->rcv_nxt))
            r.start = c->rcv_nxt;
        if (!tcp_before(r.end, merged.start) && !tcp_after(r.start, merged.end)) {
            if (tcp_before(r.start, merged.start))
                merged.start = r.start;
            if (tcp_after(r.end, merged.end))
                merged.end = r.end;
            continue;
        }
        if (count < TCP_SACK_REPORT - 1)
            retained[count++] = r;
    }
    c->sack_report[0] = merged;
    memcpy(c->sack_report + 1, retained, count * sizeof retained[0]);
    c->sack_report_count = count + 1;
}

static void sack_report_prune(struct tcp_connection *c)
{
    unsigned count = 0;
    for (unsigned i = 0; i < c->sack_report_count; i++) {
        struct tcp_range r = c->sack_report[i];
        if (!tcp_after(r.end, c->rcv_nxt))
            continue;
        if (tcp_before(r.start, c->rcv_nxt))
            r.start = c->rcv_nxt;
        c->sack_report[count++] = r;
    }
    c->sack_report_count = count;
}

/* tcp_acknowledge sends the ACK now or arms the delayed-ACK deadline once. */
void tcp_acknowledge(struct tcp_connection *c, bool immediate)
{
    if (immediate) {
        tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0);
        return;
    }
    if (!c->ack_deadline) {
        c->ack_deadline = net_clock_ms() + TCP_DELAYED_ACK_MS;
        tcp_counters.delayed_acks++;
    }
}

/* Receiver silly window avoidance (RFC 1122 section 4.2.3.3) applies after
 * a read. The window is announced on its own only when the right edge moves
 * by at least the smaller of half the store and one full segment. Every other
 * segment carries the current window anyway. */
void tcp_window_update(struct tcp_connection *c)
{
    if (c->state == TCP_CLOSED || c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED)
        return;
    uint32_t edge = c->rcv_nxt + tcp_receive_window(c);
    uint32_t threshold = MIN(TCP_RECEIVE_CAPACITY / 2, tcp_receive_mss(c));
    if (tcp_after(edge, c->rcv_adv) && edge - c->rcv_adv >= threshold)
        tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0);
}

void tcp_receive_segment(struct tcp_connection *c, const struct tcp_segment *segment)
{
    if (c->peer_fin)
        return;
    bool received_fin = false;
    spin_lock(&tcp_lock);
    uint32_t right = c->rcv_nxt + tcp_receive_window_locked(c);
    uint32_t before = c->rcv_nxt;
    bool had_hole = c->out_of_order;
    /* Without a receive store (an orphan) only a FIN is processed. */
    size_t length = c->receive ? segment->length : 0;
    for (size_t i = 0; i < length; i++) {
        uint32_t sequence = segment->sequence + i;
        if (tcp_before(sequence, c->rcv_nxt) || !tcp_before(sequence, right))
            continue;
        unsigned index = (c->receive_head + c->receive_count + (uint32_t)(sequence - c->rcv_nxt)) %
                         TCP_RECEIVE_CAPACITY;
        if (present(c, index))
            continue;
        c->receive[index] = segment->data[i];
        c->receive_present[index / 8] |= 1u << (index % 8);
        c->out_of_order++;
    }
    uint32_t fin_sequence = segment->sequence + segment->length;
    /* A FIN may lie at the right edge, because it occupies no receive space. */
    if ((segment->flags & TCP_FIN) && !tcp_before(fin_sequence, c->rcv_nxt) &&
        !tcp_after(fin_sequence, right) && !c->pending_fin) {
        c->pending_fin = true;
        c->pending_fin_sequence = fin_sequence;
    }
    for (;;) {
        if (c->pending_fin && c->pending_fin_sequence == c->rcv_nxt) {
            c->rcv_nxt++;
            c->peer_fin = true;
            c->pending_fin = false;
            if (c->out_of_order)
                memset(c->receive_present, 0, TCP_RECEIVE_CAPACITY / 8);
            c->out_of_order = 0;
            received_fin = true;
            break;
        }
        unsigned index = (c->receive_head + c->receive_count) % TCP_RECEIVE_CAPACITY;
        if (!c->out_of_order || !present(c, index))
            break;
        c->receive_present[index / 8] &= ~(1u << (index % 8));
        c->out_of_order--;
        c->receive_count++;
        c->rcv_nxt++;
    }
    if (c->read_shutdown) {
        /* Advance the ring origin by discarded contiguous bytes so retained
         * out-of-order positions retain their sequence-to-slot mapping. */
        c->receive_head = (c->receive_head + c->receive_count) % TCP_RECEIVE_CAPACITY;
        c->receive_count = 0;
    }
    bool hole = c->out_of_order;

    /* The part of this segment inside the window that is still beyond
     * rcv_nxt is out of order; the run of stored bytes around it becomes
     * the first SACK block. */
    uint32_t start = tcp_before(segment->sequence, before) ? before : segment->sequence;
    uint32_t end = segment->sequence + (uint32_t)length;
    if (tcp_after(end, right))
        end = right;
    bool report = c->sack && hole && !received_fin && length && tcp_before(start, end) &&
                  tcp_after(end, c->rcv_nxt);
    struct tcp_range run = {0, 0};
    if (report)
        run = stored_run(c, tcp_before(start, c->rcv_nxt) ? c->rcv_nxt : start, end, right);
    spin_unlock(&tcp_lock);
    if (c->sack) {
        if (report)
            sack_report(c, run.start, run.end);
        else
            sack_report_prune(c);
        if (received_fin || !hole)
            c->sack_report_count = 0;
    }
    if (received_fin) {
        switch (c->state) {
        case TCP_ESTABLISHED:
            c->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            c->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2:
            tcp_enter_time_wait(c);
            break;
        default:
            break;
        }
    }
    /* As RFC 5681 section 4.2 and RFC 1122 section 4.2.3.2 ask, a FIN, a
     * segment that arrives out of order or repeats data, and a segment that
     * fills part of a hole are acknowledged at once, as is in-order data
     * once two full segments are unacknowledged. Other in-order data waits at most
     * TCP_DELAYED_ACK_MS, and any segment we send settles the ACK earlier.
     * There is no Nagle algorithm, so a sender never waits for this ACK
     * before sending a small write. */
    if (!segment->length && !(segment->flags & TCP_FIN))
        return;
    uint32_t advanced = c->rcv_nxt - before;
    c->ack_owed += advanced;
    bool immediate = received_fin || !advanced || had_hole || hole ||
                     c->ack_owed >= 2 * tcp_receive_mss(c);
    tcp_acknowledge(c, immediate);
}
