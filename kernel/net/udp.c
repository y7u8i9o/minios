/* UDP endpoint state and receive rings are protected by udp_lock. Protocol
 * requests and input execute on netd; receive/poll inspect copied state under
 * the condition lock. No queued packet contains a socket pointer. */
#include <net/ipv4.h>
#include <net/wire.h>
#include <net/inet.h>
#include <net/worker.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <sched/wait.h>
#include <sched/sched.h>
#include <ipc/signal.h>
#include <lib/string.h>
#include <lib/random.h>
#include <errno.h>

#define UDP_SOCKETS 64
#define UDP_RECEIVE 16
#define UDP_BYTES 16384
#define UDP_MAX_PAYLOAD (PBUF_SIZE - PBUF_HEADROOM - 28)
struct datagram {
    struct pbuf *p;
    uint32_t address;
    uint16_t port;
};
struct endpoint {
    struct socket *socket;
    uint32_t local, peer, source;
    uint16_t port, peer_port;
    unsigned head, count, bytes;
    bool broadcast; /* SO_BROADCAST */
    struct datagram rx[UDP_RECEIVE];
    struct waitq wait;
};
static struct endpoint endpoints[UDP_SOCKETS];
static DEFINE_SPINLOCK(udp_lock);

uint16_t udp_checksum(uint32_t src, uint32_t dst, const void *data, size_t len)
{
    uint8_t pseudo[12];
    net_put_be32(pseudo, src);
    net_put_be32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = 17;
    net_put_be16(pseudo + 10, (uint16_t)len);
    return net_checksum_finish(
        net_checksum_partial(data, len, net_checksum_partial(pseudo, 12, 0)));
}

/* Wildcard bindings exclude every specific address on the same port.
 * Specific addresses may share a port when they differ. Reuse options are
 * unsupported, so lookup can never have two eligible receivers. */
static bool available(struct endpoint *self, uint32_t address, uint16_t port)
{
    for (unsigned i = 0; i < UDP_SOCKETS; i++) {
        struct endpoint *e = &endpoints[i];
        if (e != self && e->socket && e->port == port &&
            (!address || !e->local || e->local == address))
            return false;
    }
    return true;
}

static int bind_port(struct endpoint *e, uint32_t address, uint16_t port)
{
    if (port) {
        if (!available(e, address, port))
            return -EADDRINUSE;
    } else {
        unsigned tries;
        for (tries = 0; tries < 16384; tries++) {
            uint32_t value;
            if (random_u32(&value) < 0)
                return -EAGAIN;
            port = 49152 + (value & 16383);
            if (available(e, address, port))
                break;
        }
        if (tries == 16384)
            return -EADDRINUSE;
    }
    e->local = address;
    e->port = port;
    return 0;
}

enum operation {
    OP_BIND,
    OP_CONNECT,
    OP_SEND,
    OP_CLOSE,
};
struct udp_request {
    struct net_request request;
    struct socket *socket;
    enum operation operation;
    uint32_t address;
    uint16_t port;
    bool addressed;
    const char *data;
    size_t len;
};

/* Teardown is serialized with input on netd. Detach under the endpoint
 * lock, then free outside it: releasing packet storage takes the pool lock. */
static int close_endpoint(struct endpoint *endpoint)
{
    struct pbuf *packets[UDP_RECEIVE];

    spin_lock(&udp_lock);
    unsigned count = endpoint->count;
    for (unsigned i = 0; i < count; i++)
        packets[i] = endpoint->rx[(endpoint->head + i) % UDP_RECEIVE].p;
    endpoint->socket = NULL;
    endpoint->count = 0;
    endpoint->bytes = 0;
    spin_unlock(&udp_lock);

    for (unsigned i = 0; i < count; i++)
        pbuf_free(packets[i]);
    return 0;
}

static int bind_endpoint(struct endpoint *endpoint, uint32_t address, uint16_t port)
{
    if (address && !ipv4_local(address))
        return -EADDRNOTAVAIL;

    spin_lock(&udp_lock);
    int result = endpoint->port ? -EINVAL : bind_port(endpoint, address, port);
    spin_unlock(&udp_lock);
    return result;
}

struct udp_destination {
    uint32_t source;
    uint32_t destination;
    uint16_t source_port;
    uint16_t destination_port;
};

/* Resolve the local/remote tuple before allocating a packet. The worker
 * serializes binding and connection changes, while readers and pollers
 * use udp_lock to observe the resulting endpoint state. */
static int select_destination(struct udp_request *request, struct udp_destination *tuple)
{
    struct endpoint *endpoint = request->socket->priv;

    spin_lock(&udp_lock);
    tuple->destination = request->addressed ? request->address : endpoint->peer;
    tuple->destination_port = request->addressed ? request->port : endpoint->peer_port;
    tuple->source = endpoint->local;
    spin_unlock(&udp_lock);

    if (!tuple->destination_port || !tuple->destination)
        return -EDESTADDRREQ;

    struct net_route route;
    int result = net_route_lookup(tuple->destination, &route);
    if (result < 0)
        return result;
    if (!tuple->source)
        tuple->source = route.source;
    if (tuple->destination == IPV4_BROADCAST) {
        spin_lock(&udp_lock);
        bool allowed = endpoint->broadcast;
        spin_unlock(&udp_lock);
        if (!allowed)
            return -EACCES;
    } else if (!ipv4_local(tuple->source) ||
               (route.netif != netif_loopback() && (tuple->source >> 24) == 127)) {
        return -EADDRNOTAVAIL;
    }
    if (request->operation == OP_SEND && request->len > UDP_MAX_PAYLOAD)
        return -EMSGSIZE;

    spin_lock(&udp_lock);
    result = endpoint->port ? 0 : bind_port(endpoint, endpoint->local, 0);
    tuple->source_port = endpoint->port;
    if (!result && request->operation == OP_CONNECT) {
        endpoint->peer = tuple->destination;
        endpoint->peer_port = tuple->destination_port;
        endpoint->source = tuple->source;
    }
    spin_unlock(&udp_lock);
    return result;
}

/* A successful send transfers one complete packet to IPv4/ARP. The request
 * retains no application pointer beyond completion; the packet owns a copy. */
static int send_packet(struct udp_request *request, const struct udp_destination *tuple)
{
    int pending = socket_take_error(request->socket);
    if (pending)
        return pending;

    struct pbuf *packet = pbuf_alloc(PBUF_DATA);
    if (!packet)
        return -ENOBUFS;

    uint8_t *header = pbuf_put(packet, request->len + 8);
    net_put_be16(header, tuple->source_port);
    net_put_be16(header + 2, tuple->destination_port);
    net_put_be16(header + 4, (uint16_t)(request->len + 8));
    header[6] = 0;
    header[7] = 0;
    if (request->len)
        memcpy(header + 8, request->data, request->len);

    uint16_t checksum = udp_checksum(tuple->source, tuple->destination, header, packet->len);
    net_put_be16(header + 6, checksum ? checksum : 0xffff);
    int result = ipv4_output(packet, tuple->source, tuple->destination, IPPROTO_UDP);
    return result < 0 ? result : (int)request->len;
}

static int operate(struct net_request *request)
{
    struct udp_request *udp_request = (void *)request;
    struct endpoint *endpoint = udp_request->socket->priv;

    switch (udp_request->operation) {
    case OP_CLOSE:
        return close_endpoint(endpoint);
    case OP_BIND:
        return bind_endpoint(endpoint, udp_request->address, udp_request->port);
    case OP_CONNECT:
    case OP_SEND: {
        struct udp_destination tuple;
        int result = select_destination(udp_request, &tuple);
        if (result < 0 || udp_request->operation == OP_CONNECT)
            return result;
        return send_packet(udp_request, &tuple);
    }
    }
    return -EINVAL;
}

static int run(struct udp_request *r)
{
    net_request_init(&r->request, operate);
    return net_worker_is_current() ? operate(&r->request) : net_request_run(&r->request);
}

static int address_operation(struct socket *s,
                             const struct sockaddr_storage *addr,
                             enum operation op)
{
    const struct sockaddr_in *a = (const void *)addr;
    struct udp_request r = {
        .socket = s,
        .operation = op,
        .addressed = true,
        .address = ntohl(a->sin_addr.s_addr),
        .port = ntohs(a->sin_port),
    };
    return run(&r);
}

static int udp_bind(struct socket *s, const struct sockaddr_storage *a, socklen_t len)
{
    return address_operation(s, a, OP_BIND);
}

static int udp_connect(struct socket *s, const struct sockaddr_storage *a, socklen_t len)
{
    return address_operation(s, a, OP_CONNECT);
}

static long udp_send(struct socket *s, struct socket_msg *m)
{
    if (m->flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL))
        return -EOPNOTSUPP;
    if (m->nfiles)
        return -EOPNOTSUPP;
    if (m->len > UDP_MAX_PAYLOAD)
        return -EMSGSIZE;
    struct udp_request r = {
        .socket = s,
        .operation = OP_SEND,
        .data = m->data,
        .len = m->len,
    };
    if (m->addr) {
        if (m->addrlen < sizeof(struct sockaddr_in))
            return -EINVAL;
        if (m->addr->ss_family != AF_INET)
            return -EAFNOSUPPORT;
        const struct sockaddr_in *a = (const void *)m->addr;
        r.addressed = true;
        r.address = ntohl(a->sin_addr.s_addr);
        r.port = ntohs(a->sin_port);
    }
    return run(&r);
}

static void set_address(struct sockaddr_storage *storage, uint32_t address, uint16_t port)
{
    memset(storage, 0, sizeof *storage);
    struct sockaddr_in *a = (void *)storage;
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(address);
    a->sin_port = htons(port);
}

/* recv consumes a datagram even when its caller offers zero bytes. In
 * contrast, read(fd, ..., 0) returns in file_read before reaching here.
 * MSG_PEEK keeps ownership in the ring; ordinary receive frees after unlock. */
static long udp_receive(struct socket *s, struct socket_msg *m)
{
    if (m->flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC))
        return -EOPNOTSUPP;
    struct endpoint *e = s->priv;
    m->nfiles = 0;
    m->rflags = 0;
    m->addrlen = 0;
    spin_lock(&udp_lock);
    while (!e->count) {
        int error = socket_take_error(s);
        if (error) {
            spin_unlock(&udp_lock);
            return error;
        }
        if (m->flags & MSG_DONTWAIT) {
            spin_unlock(&udp_lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&udp_lock);
            return -EINTR;
        }
        waitq_wait(&e->wait, &udp_lock);
    }
    struct datagram d = e->rx[e->head];
    size_t copied = MIN(m->len, d.p->len), length = d.p->len;
    if (copied)
        memcpy(m->data, d.p->data, copied);
    if (m->addr) {
        set_address(m->addr, d.address, d.port);
        m->addrlen = sizeof(struct sockaddr_in);
    }
    if (copied < length)
        m->rflags |= MSG_TRUNC;
    bool consume = !(m->flags & MSG_PEEK);
    if (consume) {
        e->head = (e->head + 1) % UDP_RECEIVE;
        e->count--;
        e->bytes -= length;
    }
    spin_unlock(&udp_lock);
    if (consume)
        pbuf_free(d.p);
    return (long)((m->flags & MSG_TRUNC) ? length : copied);
}

/* Writability means the endpoint accepts a send attempt. Bounded global
 * packet, request or device pressure may still make that attempt ENOBUFS. */
static int udp_poll(struct socket *s)
{
    spin_lock(&udp_lock);
    struct endpoint *e = s->priv;
    int ready = POLLOUT | (e->count ? POLLIN : 0);
    spin_lock(&s->lock);
    if (s->error)
        ready |= POLLERR;
    spin_unlock(&s->lock);
    spin_unlock(&udp_lock);
    return ready;
}

static int udp_name(struct socket *s, struct sockaddr_storage *a, socklen_t *len, bool peer)
{
    struct endpoint *e = s->priv;
    spin_lock(&udp_lock);
    if (peer && !e->peer_port) {
        spin_unlock(&udp_lock);
        return -ENOTCONN;
    }
    struct sockaddr_storage name;
    uint32_t address = e->local ? e->local : e->source;
    uint16_t port = e->port;
    if (peer) {
        address = e->peer;
        port = e->peer_port;
    }
    set_address(&name, address, port);
    memcpy(a, &name, MIN(*len, sizeof(struct sockaddr_in)));
    *len = sizeof(struct sockaddr_in);
    spin_unlock(&udp_lock);
    return 0;
}

static void udp_release(struct socket *s)
{
    struct udp_request r = {
        .socket = s,
        .operation = OP_CLOSE,
    };
    net_request_init(&r.request, operate);
    if (net_worker_is_current()) {
        operate(&r.request);
        return;
    }
    while (net_request_submit(&r.request) == -ENOBUFS)
        sched_yield();
    net_request_finish(&r.request);
}

static int udp_setsockopt(struct socket *s, int level, int name, const void *val, socklen_t len)
{
    if (level != SOL_SOCKET || name != SO_BROADCAST)
        return -ENOPROTOOPT;
    if (len < sizeof(int))
        return -EINVAL;
    int v;
    memcpy(&v, val, sizeof v);
    struct endpoint *e = s->priv;
    spin_lock(&udp_lock);
    e->broadcast = v != 0;
    spin_unlock(&udp_lock);
    return 0;
}
static int udp_getsockopt(struct socket *s, int level, int name, void *val, socklen_t *len)
{
    if (level != SOL_SOCKET || name != SO_BROADCAST)
        return -ENOPROTOOPT;
    if (*len < sizeof(int))
        return -EINVAL;
    struct endpoint *e = s->priv;
    spin_lock(&udp_lock);
    int v = e->broadcast;
    spin_unlock(&udp_lock);
    memcpy(val, &v, sizeof v);
    *len = sizeof v;
    return 0;
}
static const struct socket_ops udp_ops = {
    .setsockopt = udp_setsockopt,
    .getsockopt = udp_getsockopt,
    .bind = udp_bind,
    .connect = udp_connect,
    .sendmsg = udp_send,
    .recvmsg = udp_receive,
    .poll = udp_poll,
    .getname = udp_name,
    .release = udp_release,
};

static int udp_create(struct socket *s)
{
    spin_lock(&udp_lock);
    for (unsigned i = 0; i < UDP_SOCKETS; i++) {
        struct endpoint *e = &endpoints[i];
        if (e->socket)
            continue;
        memset(e, 0, sizeof *e);
        waitq_init(&e->wait, "udp_receive");
        e->socket = s;
        s->priv = e;
        s->ops = &udp_ops;
        spin_unlock(&udp_lock);
        return 0;
    }
    spin_unlock(&udp_lock);
    return -ENOBUFS;
}

void udp_init(void)
{
    static const struct inet_protocol protocol = {
        .type = SOCK_DGRAM,
        .protocol = IPPROTO_UDP,
        .create = udp_create,
    };
    inet_register_protocol(&protocol);
}

void udp_input(struct netif *n, struct pbuf *p)
{
    uint8_t *ip = p->data, *h = ip + 20;
    if (p->len < 28)
        goto invalid;
    uint32_t src = net_get_be32(ip + 12), dst = net_get_be32(ip + 16);
    uint16_t length = net_get_be16(h + 4), sport = net_get_be16(h), dport = net_get_be16(h + 2);
    if (!udp_wire_valid(src, dst, h, p->len - 20))
        goto invalid;
    spin_lock(&udp_lock);
    for (unsigned i = 0; i < UDP_SOCKETS; i++) {
        struct endpoint *e = &endpoints[i];
        if (!e->socket || e->port != dport || (e->local && e->local != dst) ||
            (e->peer_port && (e->peer != src || e->peer_port != sport || e->source != dst)))
            continue;
        if (e->count == UDP_RECEIVE || e->bytes + length - 8 > UDP_BYTES) {
            net_ip_stats.udp_full++;
            spin_unlock(&udp_lock);
            pbuf_free(p);
            return;
        }
        pbuf_pull(p, 28);
        e->rx[(e->head + e->count) % UDP_RECEIVE] = (struct datagram){p, src, sport};
        e->count++;
        e->bytes += p->len;
        waitq_wake_all(&e->wait);
        poll_source_notify(&e->socket->poll);
        spin_unlock(&udp_lock);
        return;
    }
    spin_unlock(&udp_lock);
    net_ip_stats.udp_no_port++;
    icmp_error(p, 3);
    pbuf_free(p);
    return;
invalid:
    net_ip_stats.udp_invalid++;
    pbuf_free(p);
}

/* Only connected sockets receive asynchronous network errors. Validate
 * the quoted IPv4 header and match the full outbound tuple; the first send,
 * empty receive, or SO_ERROR consumes the pending error. */
void udp_icmp_error(const uint8_t *ip, size_t len, int error)
{
    if (len < 28 || ip[0] != 0x45 || ip[9] != 17 || net_checksum(ip, 20) ||
        net_get_be16(ip + 2) < 28 || (net_get_be16(ip + 6) & 0xbfff))
        return;
    uint32_t src = net_get_be32(ip + 12), dst = net_get_be32(ip + 16);
    uint16_t sport = net_get_be16(ip + 20), dport = net_get_be16(ip + 22);
    if (!ipv4_local(src))
        return;
    spin_lock(&udp_lock);
    for (unsigned i = 0; i < UDP_SOCKETS; i++) {
        struct endpoint *e = &endpoints[i];
        if (e->socket && e->peer_port && e->port == sport && e->peer_port == dport &&
            e->peer == dst && e->source == src) {
            socket_set_error(e->socket, -error);
            waitq_wake_all(&e->wait);
            poll_source_notify(&e->socket->poll);
        }
    }
    spin_unlock(&udp_lock);
}

void udp_output_error(const struct pbuf *p, int error)
{
    udp_icmp_error(p->data, p->len, error);
}
