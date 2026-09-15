/* One worker timer per connection; no protocol work runs in timer IRQs. */
#include "internal.h"
#include <errno.h>

void tcp_schedule(struct tcp_connection *c)
{
    uint64_t next = UINT64_MAX;
    if (c->control_deadline && c->control_deadline < next)
        next = c->control_deadline;
    if (c->data_deadline && c->data_deadline < next)
        next = c->data_deadline;
    if (c->lifetime_deadline && c->lifetime_deadline < next)
        next = c->lifetime_deadline;
    if (next == UINT64_MAX)
        net_timer_cancel(&c->timer);
    else
        net_timer_arm(&c->timer, next);
}

void tcp_start_control_timer(struct tcp_connection *c)
{
    c->retries = 0;
    c->control_deadline = net_clock_ms() + TCP_RETRY_MS;
    tcp_schedule(c);
}

void tcp_timer_expire(struct net_timer *timer)
{
    struct tcp_connection *c = container_of(timer, struct tcp_connection, timer);
    uint64_t now = net_clock_ms();
    if (c->lifetime_deadline && now >= c->lifetime_deadline) {
        if (c->state != TCP_TIME_WAIT) {
            tcp_counters.timeouts++;
            tcp_emit(c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0);
        }
        tcp_fail(c, c->state == TCP_TIME_WAIT ? 0 : -ETIMEDOUT);
        return;
    }
    if (c->data_deadline && now >= c->data_deadline) {
        tcp_data_timeout(c);
        if (!c->used || c->state == TCP_CLOSED)
            return;
    }
    if (c->control_deadline && now >= c->control_deadline) {
        if (c->retries == TCP_MAX_RETRIES) {
            tcp_counters.timeouts++;
            tcp_fail(c, -ETIMEDOUT);
            return;
        }
        if (c->state == TCP_SYN_SENT)
            tcp_emit(c, TCP_SYN, c->iss, NULL, 0);
        else if (c->state == TCP_SYN_RECEIVED)
            tcp_emit(c, TCP_SYN | TCP_ACK, c->iss, NULL, 0);
        else if (c->fin_sent)
            tcp_emit(c, TCP_FIN | TCP_ACK, c->fin_sequence, NULL, 0);
        c->retries++;
        tcp_counters.retransmits++;
        c->control_deadline = now + ((uint64_t)TCP_RETRY_MS << c->retries);
    }
    tcp_schedule(c);
}
