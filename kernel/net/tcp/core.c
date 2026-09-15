/* Connection ownership, port reservations and state publication. Protocol
 * transitions execute on netd; socket consumers observe only locked views. */
#include "internal.h"
#include <net/inet.h>
#include <lib/string.h>
#include <lib/random.h>
#include <errno.h>
#include <kassert.h>

struct spinlock tcp_lock = SPINLOCK_INIT("tcp_endpoints");
struct tcp_endpoint tcp_endpoints[TCP_ENDPOINTS];
struct tcp_connection tcp_connections[TCP_CONNECTIONS];
struct tcp_stats tcp_counters;
static uint64_t accept_order;
static uint32_t (*sequence_generator)(void);
static uint16_t (*port_generator)(void);

void tcp_set_generators(uint32_t (*sequence)(void), uint16_t (*port)(void))
{
    kassert(net_worker_is_current());
    sequence_generator = sequence;
    port_generator = port;
}

uint32_t tcp_initial_sequence(void)
{
    if (sequence_generator)
        return sequence_generator();
    uint32_t sequence;
    kassert(random_u32(&sequence) == 0);
    return sequence;
}

static bool port_available(struct tcp_endpoint *self, uint32_t address, uint16_t port)
{
    /* Caller holds tcp_lock. Closing connections, including TIME_WAIT,
     * reserve the local port independently of their former socket file. */
    for (unsigned i = 0; i < TCP_ENDPOINTS; i++) {
        struct tcp_endpoint *e = &tcp_endpoints[i];
        if (e != self && e->socket && e->local_port == port &&
            (!address || !e->local_address || e->local_address == address))
            return false;
    }
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (c->used && c->endpoint != self && c->local_port == port &&
            (!address || c->local_address == address))
            return false;
    }
    return true;
}

int tcp_bind_port(struct tcp_endpoint *endpoint, uint32_t address, uint16_t port)
{
    if (address && !ipv4_local(address))
        return -EADDRNOTAVAIL;
    spin_lock(&tcp_lock);
    if (port) {
        if (!port_available(endpoint, address, port)) {
            spin_unlock(&tcp_lock);
            return -EADDRINUSE;
        }
    } else {
        unsigned attempts;
        for (attempts = 0; attempts < 16384; attempts++) {
            if (port_generator) {
                port = port_generator();
            } else {
                uint32_t value;
                if (random_u32(&value) < 0) {
                    spin_unlock(&tcp_lock);
                    return -EAGAIN;
                }
                port = 49152 + (value & 16383);
            }
            if (port >= 49152 && port_available(endpoint, address, port))
                break;
        }
        if (attempts == 16384) {
            spin_unlock(&tcp_lock);
            return -EADDRINUSE;
        }
    }
    endpoint->local_address = address;
    endpoint->local_port = port;
    spin_unlock(&tcp_lock);
    return 0;
}

struct tcp_connection *tcp_connection_alloc(void)
{
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (c->used)
            continue;
        memset(c, 0, sizeof *c);
        c->used = true;
        c->peer_mss = TCP_DEFAULT_MSS;
        c->local_mss = TCP_LOCAL_MSS;
        net_timer_init(&c->timer, tcp_timer_expire);
        return c;
    }
    return NULL;
}

void tcp_listener_counts(struct tcp_endpoint *listener, unsigned *incomplete, unsigned *ready)
{
    *incomplete = 0;
    *ready = 0;
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (!c->used || c->listener != listener)
            continue;
        if (c->accept_ready)
            (*ready)++;
        else
            (*incomplete)++;
    }
}

void tcp_publish_listener(struct tcp_endpoint *listener)
{
    unsigned incomplete, ready;
    tcp_listener_counts(listener, &incomplete, &ready);
    spin_lock(&tcp_lock);
    listener->pending = ready;
    waitq_wake_all(&listener->wait);
    poll_source_notify(&listener->socket->poll);
    spin_unlock(&tcp_lock);
}

void tcp_connection_free(struct tcp_connection *c)
{
    kassert(!c->endpoint);
    struct tcp_endpoint *listener = c->listener;
    net_timer_cancel(&c->timer);
    c->listener = NULL;
    c->used = false;
    if (listener)
        tcp_publish_listener(listener);
}

unsigned tcp_receive_window(struct tcp_connection *c)
{
    spin_lock(&tcp_lock);
    unsigned available = c->read_shutdown
                             ? TCP_RECEIVE_CAPACITY
                             : TCP_RECEIVE_CAPACITY - c->receive_count - c->out_of_order;
    spin_unlock(&tcp_lock);
    return available;
}

void tcp_publish(struct tcp_connection *c)
{
    struct tcp_endpoint *e = c->endpoint;
    if (!e)
        return;
    spin_lock(&tcp_lock);
    e->connecting = c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED;
    e->connected = c->state >= TCP_ESTABLISHED;
    e->eof = c->peer_fin || c->read_shutdown || c->state == TCP_CLOSED;
    e->write_closed = c->fin_requested || c->state == TCP_CLOSED;
    e->writable = (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) &&
                  !c->fin_requested && c->transmit_length < TCP_SEND_CAPACITY;
    e->terminal_error = c->error;
    if (c->error)
        socket_set_error(e->socket, c->error);
    waitq_wake_all(&e->wait);
    poll_source_notify(&e->socket->poll);
    spin_unlock(&tcp_lock);
}

void tcp_fail(struct tcp_connection *c, int error)
{
    c->state = TCP_CLOSED;
    c->error = error;
    c->control_deadline = 0;
    c->data_deadline = 0;
    c->lifetime_deadline = 0;
    c->transmit_length = 0;
    c->transmit_sent = 0;
    c->progress_deadline = 0;
    net_timer_cancel(&c->timer);
    tcp_publish(c);
    if (!c->endpoint)
        tcp_connection_free(c);
}

void tcp_established(struct tcp_connection *c)
{
    c->state = TCP_ESTABLISHED;
    c->control_deadline = 0;
    c->snd_una = c->snd_nxt;
    tcp_transfer_init(c);
    tcp_counters.established++;
    if (c->listener) {
        unsigned incomplete, ready;
        tcp_listener_counts(c->listener, &incomplete, &ready);
        if (ready >= c->listener->backlog) {
            tcp_counters.backlog_drops++;
            tcp_emit(c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0);
            tcp_fail(c, -ECONNABORTED);
            return;
        }
        c->accept_ready = true;
        c->accept_order = ++accept_order;
        c->lifetime_deadline = net_clock_ms() + TCP_ORPHAN_MS;
        tcp_publish_listener(c->listener);
    }
    tcp_schedule(c);
    tcp_publish(c);
}

void tcp_enter_time_wait(struct tcp_connection *c)
{
    c->state = TCP_TIME_WAIT;
    c->control_deadline = 0;
    c->data_deadline = 0;
    c->lifetime_deadline = net_clock_ms() + TCP_TIME_WAIT_MS;
    tcp_schedule(c);
    tcp_publish(c);
}

void tcp_maybe_fin(struct tcp_connection *c)
{
    if (!c->fin_requested || c->fin_sent || c->transmit_length ||
        (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
        return;
    c->fin_sequence = c->snd_nxt++;
    c->fin_sent = true;
    c->state = c->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
    tcp_emit(c, TCP_FIN | TCP_ACK, c->fin_sequence, NULL, 0);
    tcp_start_control_timer(c);
    tcp_publish(c);
}

struct stats_request {
    struct net_request request;
    struct tcp_stats *out;
};
static int snapshot(struct net_request *request)
{
    struct tcp_stats result = tcp_counters;
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (!c->used)
            continue;
        result.connections++;
        result.half_open += c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED;
        result.accepted += c->accept_ready;
        result.time_wait += c->state == TCP_TIME_WAIT;
    }
    spin_lock(&tcp_lock);
    for (unsigned i = 0; i < TCP_ENDPOINTS; i++)
        result.endpoints += tcp_endpoints[i].socket != NULL;
    spin_unlock(&tcp_lock);
    *((struct stats_request *)request)->out = result;
    return 0;
}
int tcp_get_stats(struct tcp_stats *out)
{
    struct stats_request r = {.out = out};
    net_request_init(&r.request, snapshot);
    if (net_worker_is_current())
        return snapshot(&r.request);
    return net_request_run(&r.request);
}

void tcp_init(void)
{
    static const struct inet_protocol protocol = {
        .type = SOCK_STREAM,
        .protocol = IPPROTO_TCP,
        .create = tcp_endpoint_create,
    };
    inet_register_protocol(&protocol);
}

bool tcp_random_ready(void)
{
    return sequence_generator || random_ready();
}
