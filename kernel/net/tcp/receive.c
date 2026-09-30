/* One circular receive store serves both contiguous and out-of-order data.
 * Existing bytes win overlaps. Missing bytes can fill holes without copying
 * or delivering an already accepted byte twice. */
#include "internal.h"
#include <lib/string.h>

static bool present(struct tcp_connection *c, unsigned index)
{
    return c->receive_present[index / 8] & (1u << (index % 8));
}

void tcp_receive_discard(struct tcp_connection *c)
{
    /* Caller holds tcp_lock. */
    c->receive_count = 0;
    c->out_of_order = 0;
    c->pending_fin = false;
    if (c->receive_present)
        memset(c->receive_present, 0, TCP_RECEIVE_CAPACITY / 8);
}

void tcp_receive_segment(struct tcp_connection *c, const struct tcp_segment *segment)
{
    if (c->peer_fin)
        return;
    bool received_fin = false;
    spin_lock(&tcp_lock);
    uint32_t right = c->rcv_nxt + tcp_receive_window_locked(c);
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
    if ((segment->flags & TCP_FIN) && !tcp_before(fin_sequence, c->rcv_nxt) &&
        tcp_before(fin_sequence, right) && !c->pending_fin) {
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
         * out-of-order positions keep their sequence-to-slot mapping. */
        c->receive_head = (c->receive_head + c->receive_count) % TCP_RECEIVE_CAPACITY;
        c->receive_count = 0;
    }
    spin_unlock(&tcp_lock);
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
    /* Immediate ACKs and immediate small writes avoid a Nagle/delayed-ACK
     * dependency. Duplicate and out-of-order input also refreshes the ACK. */
    if (segment->length || (segment->flags & TCP_FIN))
        tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0);
}
