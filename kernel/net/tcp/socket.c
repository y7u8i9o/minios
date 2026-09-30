/* Socket-facing TCP operations. Files keep endpoints alive during requests;
 * endpoint waiters sleep only after checking readiness under tcp_lock. */
#include "internal.h"
#include <net/byteorder.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <ipc/signal.h>
#include <lib/string.h>
#include <errno.h>

enum request_operation {
    REQUEST_BIND,
    REQUEST_LISTEN,
    REQUEST_CONNECT,
    REQUEST_ACCEPT,
    REQUEST_SEND,
    REQUEST_SHUTDOWN,
    REQUEST_WINDOW,
    REQUEST_CLOSE,
};
struct tcp_request {
    struct net_request request;
    struct tcp_endpoint *endpoint;
    enum request_operation operation;
    uint32_t address;
    uint16_t port;
    int argument;
    const char *data;
    size_t length;
    struct socket *accepted;
};

int tcp_endpoint_create(struct socket *socket)
{
    spin_lock(&tcp_lock);
    for (unsigned i = 0; i < TCP_ENDPOINTS; i++) {
        struct tcp_endpoint *e = &tcp_endpoints[i];
        if (e->socket)
            continue;
        memset(e, 0, sizeof *e);
        waitq_init(&e->wait, "tcp_endpoint");
        e->socket = socket;
        socket->priv = e;
        socket->ops = &tcp_socket_ops;
        spin_unlock(&tcp_lock);
        return 0;
    }
    spin_unlock(&tcp_lock);
    return -ENOBUFS;
}

static int start_connect(struct tcp_request *request)
{
    struct tcp_endpoint *e = request->endpoint;
    if (e->listening)
        return -EOPNOTSUPP;
    if (e->connection) {
        struct tcp_connection *c = e->connection;
        if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED)
            return -EALREADY;
        return c->error ? c->error : -EISCONN;
    }
    if (!tcp_random_ready())
        return -EAGAIN;
    if (!request->port || !request->address)
        return -EINVAL;
    struct net_route route;
    int result = net_route_lookup(request->address, &route);
    if (result < 0)
        return result;
    uint32_t local = e->local_address ? e->local_address : route.source;
    if (!ipv4_local(local) || (route.netif != netif_loopback() && (local >> 24) == 127))
        return -EADDRNOTAVAIL;
    if (!e->local_port) {
        result = tcp_bind_port(e, e->local_address, 0);
        if (result < 0)
            return result;
    }
    struct tcp_connection *c = tcp_connection_alloc();
    if (!c)
        return -ENOBUFS;
    c->endpoint = e;
    c->state = TCP_SYN_SENT;
    c->local_address = local;
    c->peer_address = request->address;
    c->local_port = e->local_port;
    c->peer_port = request->port;
    c->iss = tcp_initial_sequence();
    c->ts_offset = tcp_initial_sequence();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->local_mss = MIN(TCP_LOCAL_MSS, route.netif->mtu - 40);
    spin_lock(&tcp_lock);
    e->connection = c;
    e->local_address = local;
    e->peer_address = c->peer_address;
    e->peer_port = c->peer_port;
    spin_unlock(&tcp_lock);
    tcp_counters.active_opens++;
    tcp_publish(c);
    result = tcp_emit(c, TCP_SYN, c->iss, NULL, 0);
    if (result < 0) {
        tcp_fail(c, result);
        return result;
    }
    tcp_start_control_timer(c);
    return 0;
}

static int accept_connection(struct tcp_request *request)
{
    struct tcp_endpoint *listener = request->endpoint;
    if (!listener->listening)
        return -EINVAL;
    struct tcp_connection *selected = NULL;
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_connections[i];
        if (c->used && c->listener == listener && c->accept_ready &&
            (!selected || c->accept_order < selected->accept_order))
            selected = c;
    }
    if (!selected)
        return -EAGAIN;
    struct socket *socket = socket_alloc(AF_INET, SOCK_STREAM, IPPROTO_TCP, &tcp_socket_ops);
    if (!socket)
        return -ENOMEM;
    int result = tcp_endpoint_create(socket);
    if (result < 0) {
        socket_free(socket);
        return result;
    }
    struct tcp_endpoint *endpoint = socket->priv;
    selected->listener = NULL;
    selected->accept_ready = false;
    selected->endpoint = endpoint;
    selected->lifetime_deadline = 0;
    spin_lock(&tcp_lock);
    endpoint->connection = selected;
    endpoint->local_address = selected->local_address;
    endpoint->local_port = selected->local_port;
    endpoint->peer_address = selected->peer_address;
    endpoint->peer_port = selected->peer_port;
    spin_unlock(&tcp_lock);
    tcp_schedule(selected);
    tcp_publish(selected);
    tcp_publish_listener(listener);
    request->accepted = socket;
    return 0;
}

static int send_data(struct tcp_request *request)
{
    struct tcp_connection *c = request->endpoint->connection;
    if (!c)
        return -ENOTCONN;
    if (c->error)
        return c->error;
    if (c->fin_requested)
        return -EPIPE;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
        return -ENOTCONN;
    if (!request->length)
        return 0;
    size_t available = TCP_SEND_CAPACITY - c->transmit_length;
    if (!available)
        return -EAGAIN;
    size_t length = MIN(request->length, available);
    memcpy(c->transmit + c->transmit_length, request->data, length);
    if (!c->transmit_length)
        c->progress_deadline = net_clock_ms() + TCP_PROGRESS_MS;
    c->transmit_length += length;
    tcp_flush(c);
    tcp_schedule(c);
    tcp_publish(c);
    return (int)length;
}

static int shutdown_connection(struct tcp_endpoint *e, int how)
{
    struct tcp_connection *c = e->connection;
    if (!c || c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED)
        return -ENOTCONN;
    if (how == SHUT_RD || how == SHUT_RDWR) {
        spin_lock(&tcp_lock);
        c->read_shutdown = true;
        tcp_receive_discard(c);
        spin_unlock(&tcp_lock);
        if (c->state != TCP_CLOSED)
            tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0);
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        c->fin_requested = true;
        tcp_maybe_fin(c);
    }
    tcp_publish(c);
    return 0;
}

static void close_endpoint(struct tcp_endpoint *endpoint)
{
    /* No other file reference exists. Remove queued listener children before
     * retiring the endpoint so their timers cannot notify a reused slot. */
    if (endpoint->listening) {
        for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
            struct tcp_connection *c = &tcp_connections[i];
            if (!c->used || c->listener != endpoint)
                continue;
            tcp_emit(c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0);
            tcp_connection_free(c);
        }
    }
    struct tcp_connection *c = endpoint->connection;
    if (c) {
        c->endpoint = NULL;
        if (c->state == TCP_CLOSED || c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED) {
            tcp_connection_free(c);
        } else if (c->receive_count) {
            /* Closing with unread data aborts rather than pretending it was
             * delivered to an application that no longer exists. */
            tcp_emit(c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0);
            tcp_connection_free(c);
        } else {
            c->read_shutdown = true;
            c->fin_requested = true;
            tcp_maybe_fin(c);
            if (c->state != TCP_TIME_WAIT)
                c->lifetime_deadline = net_clock_ms() + TCP_ORPHAN_MS;
            tcp_release_unused(c);
            tcp_schedule(c);
        }
    }
    spin_lock(&tcp_lock);
    endpoint->connection = NULL;
    endpoint->socket = NULL;
    endpoint->listening = false;
    spin_unlock(&tcp_lock);
}

static int perform_request(struct net_request *request)
{
    struct tcp_request *r = (void *)request;
    struct tcp_endpoint *e = r->endpoint;
    switch (r->operation) {
    case REQUEST_BIND:
        if (e->local_port || e->connection || e->listening)
            return -EINVAL;
        return tcp_bind_port(e, r->address, r->port);
    case REQUEST_LISTEN: {
        if (!tcp_random_ready())
            return -EAGAIN;
        if (e->connection)
            return -EINVAL;
        int result = e->local_port ? 0 : tcp_bind_port(e, e->local_address, 0);
        if (result < 0)
            return result;
        spin_lock(&tcp_lock);
        e->backlog = MIN(MAX(r->argument, 1), TCP_ACCEPT_BACKLOG);
        e->listening = true;
        spin_unlock(&tcp_lock);
        return 0;
    }
    case REQUEST_CONNECT:
        return start_connect(r);
    case REQUEST_ACCEPT:
        return accept_connection(r);
    case REQUEST_SEND:
        return send_data(r);
    case REQUEST_SHUTDOWN:
        return shutdown_connection(e, r->argument);
    case REQUEST_WINDOW:
        if (e->connection && e->connection->state != TCP_CLOSED)
            tcp_emit(e->connection, TCP_ACK, e->connection->snd_nxt, NULL, 0);
        return 0;
    case REQUEST_CLOSE:
        close_endpoint(e);
        return 0;
    }
    return -EINVAL;
}

static int run_request(struct tcp_request *request)
{
    net_request_init(&request->request, perform_request);
    if (net_worker_is_current())
        return perform_request(&request->request);
    return net_request_run(&request->request);
}

static int tcp_bind(struct socket *socket, const struct sockaddr_storage *storage, socklen_t length)
{
    const struct sockaddr_in *address = (const void *)storage;
    struct tcp_request request = {
        .endpoint = socket->priv,
        .operation = REQUEST_BIND,
        .address = ntohl(address->sin_addr.s_addr),
        .port = ntohs(address->sin_port),
    };
    return run_request(&request);
}

static int tcp_listen(struct socket *socket, int backlog)
{
    struct tcp_request request = {
        .endpoint = socket->priv,
        .operation = REQUEST_LISTEN,
        .argument = backlog,
    };
    return run_request(&request);
}

static int tcp_connect(struct socket *socket,
                       const struct sockaddr_storage *storage,
                       socklen_t length)
{
    const struct sockaddr_in *address = (const void *)storage;
    struct tcp_endpoint *endpoint = socket->priv;
    struct tcp_request request = {
        .endpoint = endpoint,
        .operation = REQUEST_CONNECT,
        .address = ntohl(address->sin_addr.s_addr),
        .port = ntohs(address->sin_port),
    };
    int result = run_request(&request);
    if (result < 0)
        return result;
    if (socket_nonblocking(socket))
        return -EINPROGRESS;
    spin_lock(&tcp_lock);
    while (endpoint->connecting) {
        if (signal_should_interrupt()) {
            spin_unlock(&tcp_lock);
            return -EINTR;
        }
        waitq_wait(&endpoint->wait, &tcp_lock);
    }
    result = endpoint->terminal_error;
    spin_unlock(&tcp_lock);
    return result;
}

static int tcp_accept(struct socket *socket, struct socket **out)
{
    struct tcp_endpoint *endpoint = socket->priv;
    for (;;) {
        struct tcp_request request = {
            .endpoint = endpoint,
            .operation = REQUEST_ACCEPT,
        };
        int result = run_request(&request);
        if (result != -EAGAIN) {
            if (!result)
                *out = request.accepted;
            return result;
        }
        if (socket_nonblocking(socket))
            return -EAGAIN;
        spin_lock(&tcp_lock);
        while (!endpoint->pending && endpoint->listening) {
            if (signal_should_interrupt()) {
                spin_unlock(&tcp_lock);
                return -EINTR;
            }
            waitq_wait(&endpoint->wait, &tcp_lock);
        }
        spin_unlock(&tcp_lock);
    }
}

static long tcp_send(struct socket *socket, struct socket_msg *message)
{
    if (message->flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL))
        return -EOPNOTSUPP;
    if (message->addrlen)
        return -EISCONN;
    if (message->nfiles)
        return -EOPNOTSUPP;
    struct tcp_endpoint *endpoint = socket->priv;
    for (;;) {
        struct tcp_request request = {
            .endpoint = endpoint,
            .operation = REQUEST_SEND,
            .data = message->data,
            .length = message->len,
        };
        int result = run_request(&request);
        if (result != -EAGAIN) {
            if (result == -EPIPE && !(message->flags & MSG_NOSIGNAL))
                signal_send(thread_current()->proc, SIGPIPE);
            return result;
        }
        if (message->flags & MSG_DONTWAIT)
            return -EAGAIN;
        spin_lock(&tcp_lock);
        while (!endpoint->writable && !endpoint->write_closed && !endpoint->terminal_error) {
            if (signal_should_interrupt()) {
                spin_unlock(&tcp_lock);
                return -EINTR;
            }
            waitq_wait(&endpoint->wait, &tcp_lock);
        }
        spin_unlock(&tcp_lock);
    }
}

static long tcp_receive(struct socket *socket, struct socket_msg *message)
{
    if (message->flags & ~(MSG_DONTWAIT | MSG_PEEK))
        return -EOPNOTSUPP;
    message->nfiles = 0;
    message->addrlen = 0;
    if (!message->len)
        return 0;
    struct tcp_endpoint *endpoint = socket->priv;
    spin_lock(&tcp_lock);
    for (;;) {
        struct tcp_connection *c = endpoint->connection;
        if (c && c->receive_count) {
            size_t length = MIN(message->len, c->receive_count);
            for (size_t i = 0; i < length; i++)
                message->data[i] = c->receive[(c->receive_head + i) % TCP_RECEIVE_CAPACITY];
            bool consume = !(message->flags & MSG_PEEK);
            if (consume) {
                c->receive_head = (c->receive_head + length) % TCP_RECEIVE_CAPACITY;
                c->receive_count -= length;
            }
            spin_unlock(&tcp_lock);
            if (consume) {
                struct tcp_request request = {
                    .endpoint = endpoint,
                    .operation = REQUEST_WINDOW,
                };
                /* Data is already delivered. A failed window-update request
                 * cannot turn that completed read into an error. */
                run_request(&request);
            }
            return (long)length;
        }
        int result;
        if (endpoint->terminal_error)
            result = endpoint->terminal_error;
        else if (endpoint->eof)
            result = 0;
        else if (!c || endpoint->listening)
            result = -ENOTCONN;
        else if (message->flags & MSG_DONTWAIT)
            result = -EAGAIN;
        else if (signal_should_interrupt())
            result = -EINTR;
        else {
            waitq_wait(&endpoint->wait, &tcp_lock);
            continue;
        }
        spin_unlock(&tcp_lock);
        return result;
    }
}

static int tcp_shutdown(struct socket *socket, int how)
{
    struct tcp_request request = {
        .endpoint = socket->priv,
        .operation = REQUEST_SHUTDOWN,
        .argument = how,
    };
    return run_request(&request);
}

static int tcp_poll(struct socket *socket)
{
    struct tcp_endpoint *e = socket->priv;
    spin_lock(&tcp_lock);
    int events = 0;
    if (e->listening) {
        if (e->pending)
            events |= POLLIN;
    } else if (!e->connecting) {
        if ((e->connection && e->connection->receive_count) || e->eof)
            events |= POLLIN;
        if (e->writable || e->terminal_error)
            events |= POLLOUT;
        if (!e->connection || e->terminal_error || (e->eof && e->write_closed))
            events |= POLLHUP;
    }
    spin_lock(&socket->lock);
    if (socket->error)
        events |= POLLERR;
    spin_unlock(&socket->lock);
    spin_unlock(&tcp_lock);
    return events;
}

static int tcp_name(struct socket *socket,
                    struct sockaddr_storage *storage,
                    socklen_t *length,
                    bool peer)
{
    struct tcp_endpoint *e = socket->priv;
    struct sockaddr_in address = {.sin_family = AF_INET};
    spin_lock(&tcp_lock);
    if (peer && !e->connected) {
        spin_unlock(&tcp_lock);
        return -ENOTCONN;
    }
    address.sin_addr.s_addr = htonl(peer ? e->peer_address : e->local_address);
    address.sin_port = htons(peer ? e->peer_port : e->local_port);
    spin_unlock(&tcp_lock);
    memcpy(storage, &address, MIN(*length, sizeof address));
    *length = sizeof address;
    return 0;
}

static void tcp_release(struct socket *socket)
{
    struct tcp_request request = {
        .endpoint = socket->priv,
        .operation = REQUEST_CLOSE,
    };
    net_request_init(&request.request, perform_request);
    if (net_worker_is_current()) {
        perform_request(&request.request);
        return;
    }
    while (net_request_submit(&request.request) == -ENOBUFS)
        sched_yield();
    net_request_finish(&request.request);
}

const struct socket_ops tcp_socket_ops = {
    .bind = tcp_bind,
    .listen = tcp_listen,
    .connect = tcp_connect,
    .accept = tcp_accept,
    .sendmsg = tcp_send,
    .recvmsg = tcp_receive,
    .shutdown = tcp_shutdown,
    .poll = tcp_poll,
    .getname = tcp_name,
    .release = tcp_release,
};
