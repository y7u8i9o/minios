/* Deterministic N06 wire tests. A fake Ethernet device records production
 * output; injected segments pass through the real IPv4 and TCP parsers. */
#include <console.h>
#include <errno.h>
#include "net_helpers.h"
#include "../net/tcp/internal.h"

#define LOCAL 0x0a000002u
#define PEER 0x0a000001u

static uint8_t last_frame[1600];
static unsigned frame_count;
static size_t frame_length;
static const uint8_t peer_mac[6] = {2, 0, 0, 0, 0, 1};

static int capture(struct netif *interface, struct pbuf *packet)
{
    frame_length = MIN(packet->len, sizeof last_frame);
    memcpy(last_frame, packet->data, frame_length);
    frame_count++;
    pbuf_free(packet);
    return 0;
}
static const struct netif_ops fake_ops = {
    .output = capture,
    .input = ethernet_input,
};
static struct netif fake = {
    .name = "tcptest",
    .flags = NETIF_ETHERNET,
    .mtu = 1500,
    .hwaddr = {2, 0, 0, 0, 0, 2},
    .ops = &fake_ops,
};

static uint32_t initial_sequence(void)
{
    return 0xfffffff0u;
}
static uint16_t next_port(void)
{
    static uint16_t port = 50000;
    return port++;
}

static void resolve_peer(void)
{
    struct pbuf *packet = pbuf_alloc(PBUF_CONTROL);
    ktest_assert(packet, "ARP packet allocation");
    uint8_t *header = pbuf_put(packet, 28);
    memset(header, 0, 28);
    net_put_be16(header, 1);
    net_put_be16(header + 2, 0x0800);
    header[4] = 6;
    header[5] = 4;
    net_put_be16(header + 6, 2);
    memcpy(header + 8, peer_mac, 6);
    net_put_be32(header + 14, PEER);
    memcpy(header + 18, fake.hwaddr, 6);
    net_put_be32(header + 24, LOCAL);
    arp_input(&fake, packet);
}

static struct pbuf *segment(uint16_t source_port,
                            uint16_t destination_port,
                            uint32_t sequence,
                            uint32_t acknowledgement,
                            uint8_t flags,
                            const char *data,
                            size_t length)
{
    size_t header_length = flags & TCP_SYN ? 24 : 20;
    struct pbuf *packet = ip_packet(PEER, LOCAL, IPPROTO_TCP, header_length + length);
    uint8_t *header = packet->data + 20;
    net_put_be16(header, source_port);
    net_put_be16(header + 2, destination_port);
    net_put_be32(header + 4, sequence);
    net_put_be32(header + 8, acknowledgement);
    header[12] = (header_length / 4) << 4;
    header[13] = flags;
    net_put_be16(header + 14, 4096);
    if (flags & TCP_SYN) {
        header[20] = 2;
        header[21] = 4;
        net_put_be16(header + 22, 1200);
    }
    if (length)
        memcpy(header + header_length, data, length);
    net_put_be16(header + 16, tcp_checksum(PEER, LOCAL, header, header_length + length));
    return packet;
}

static void inject(struct tcp_connection *connection,
                   uint32_t sequence,
                   uint32_t acknowledgement,
                   uint8_t flags,
                   const char *data,
                   size_t length)
{
    ipv4_input(&fake,
               segment(connection->peer_port,
                       connection->local_port,
                       sequence,
                       acknowledgement,
                       flags,
                       data,
                       length));
}

static void expect_output(uint8_t flags, uint32_t sequence, uint32_t acknowledgement)
{
    ktest_assert(frame_length >= 54 && net_get_be16(last_frame + 12) == 0x0800 &&
                     last_frame[23] == IPPROTO_TCP,
                 "TCP Ethernet output");
    const uint8_t *header = last_frame + 34;
    size_t length = net_get_be16(last_frame + 16) - 20;
    ktest_assert(header[13] == flags && net_get_be32(header + 4) == sequence &&
                     net_get_be32(header + 8) == acknowledgement,
                 "wire flags/sequence/ack %x/%x/%x expected %x/%x/%x",
                 header[13],
                 net_get_be32(header + 4),
                 net_get_be32(header + 8),
                 flags,
                 sequence,
                 acknowledgement);
    ktest_assert(tcp_checksum(LOCAL, PEER, header, length) == 0, "output TCP checksum");
}

static struct file *new_socket(void)
{
    struct file *file = NULL;
    ktest_assert(socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, O_NONBLOCK, &file) == 0,
                 "create TCP endpoint");
    return file;
}
static struct tcp_connection *connection_of(struct file *file)
{
    struct tcp_endpoint *endpoint = socket_from_file(file)->priv;
    return endpoint->connection;
}
static struct file *active_open(void)
{
    struct file *file = new_socket();
    struct sockaddr_storage name;
    address(&name, PEER, 9000);
    ktest_assert(socket_connect(socket_from_file(file), &name, 16) == -EINPROGRESS,
                 "nonblocking connect starts SYN");
    resolve_peer();
    return file;
}
static void establish(struct tcp_connection *connection)
{
    inject(connection, 100, connection->snd_nxt, TCP_SYN | TCP_ACK, NULL, 0);
    ktest_assert(connection->state == TCP_ESTABLISHED && connection->peer_mss == 1200,
                 "SYN ACK establishes with peer MSS");
}
static void expect_error(struct socket *socket, int expected)
{
    int error = -1;
    socklen_t length = sizeof error;
    ktest_assert(socket_getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                     error == expected,
                 "SO_ERROR %d expected %d",
                 error,
                 expected);
    length = sizeof error;
    ktest_assert(socket_getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error,
                 "SO_ERROR is consumed once");
}

/* Fire an armed production callback at exactly its controlled deadline.
 * Worker timer dispatch itself is covered by the N02 timer tests. */
static void expire_next(struct tcp_connection *connection)
{
    ktest_assert(net_timer_armed(&connection->timer), "connection timer armed");
    net_clock_set(connection->timer.deadline_ms);
    net_timer_cancel(&connection->timer);
    tcp_timer_expire(&connection->timer);
}

static void active_checks(void)
{
    struct file *file = active_open();
    struct socket *socket = socket_from_file(file);
    struct tcp_connection *connection = connection_of(file);
    uint32_t iss = connection->iss;
    expect_output(TCP_SYN, iss, 0);
    ktest_assert(!(socket->ops->poll(socket) & POLLOUT), "connect pending is not writable");
    struct sockaddr_storage name;
    address(&name, PEER, 9000);
    ktest_assert(socket_connect(socket, &name, 16) == -EALREADY, "duplicate pending connect");

    expire_next(connection);
    expect_output(TCP_SYN, iss, 0);
    ktest_assert(connection->retries == 1, "lost SYN retransmits original sequence");
    inject(connection, 100, iss, TCP_SYN | TCP_ACK, NULL, 0);
    ktest_assert(connection->state == TCP_SYN_SENT, "invalid handshake ACK cannot connect");
    establish(connection);
    expect_output(TCP_ACK, iss + 1, 101);
    ktest_assert(socket->ops->poll(socket) & POLLOUT, "connect completion writable");
    expect_error(socket, 0);
    inject(connection, 100, iss + 1, TCP_SYN | TCP_ACK, NULL, 0);
    expect_output(TCP_ACK, iss + 1, 101);
    ktest_assert(connection->state == TCP_ESTABLISHED, "lost final ACK recovered");

    char data[32] = "crossing 32-bit sequence wrap";
    struct socket_msg message = {
        .data = data,
        .len = sizeof data,
        .flags = MSG_NOSIGNAL,
    };
    ktest_assert(socket_sendmsg(socket, &message) == sizeof data, "send across sequence wrap");
    ktest_assert(connection->snd_nxt == 17 && (socket->ops->poll(socket) & POLLOUT),
                 "wrapped segment retains space for subsequent writes");
    inject(connection, 101, 17, TCP_ACK, NULL, 0);
    ktest_assert(connection->snd_una == 17 && !connection->transmit_length,
                 "wrapped ACK advances send state");

    /* The unscaled window is 65535 bytes since N13; this RST lies beyond it. */
    unsigned before = frame_count;
    inject(connection, 101 + 70000, 17, TCP_RST, NULL, 0);
    ktest_assert(connection->state == TCP_ESTABLISHED && frame_count == before,
                 "out-of-window RST ignored");
    inject(connection, 102, 17, TCP_RST, NULL, 0);
    expect_output(TCP_ACK, 17, 101);
    ktest_assert(connection->state == TCP_ESTABLISHED, "in-window inexact RST challenged");
    inject(connection, 101, 17, TCP_RST, NULL, 0);
    ktest_assert(connection->state == TCP_CLOSED && (socket->ops->poll(socket) & POLLERR),
                 "exact RST wakes failed endpoint");
    expect_error(socket, ECONNRESET);
    file_put(file);

    file = active_open();
    connection = connection_of(file);
    inject(connection, 0, connection->snd_nxt, TCP_RST | TCP_ACK, NULL, 0);
    expect_error(socket_from_file(file), ECONNREFUSED);
    file_put(file);

    file = active_open();
    connection = connection_of(file);
    for (unsigned retry = 0; retry < TCP_MAX_RETRIES + 1; retry++)
        expire_next(connection);
    ktest_assert(connection->state == TCP_CLOSED && connection->retries == TCP_MAX_RETRIES,
                 "bounded SYN retry budget");
    expect_error(socket_from_file(file), ETIMEDOUT);
    file_put(file);

    /* Both peers actively open: crossing SYN then SYN ACK uses one TCB. */
    file = active_open();
    connection = connection_of(file);
    inject(connection, 100, 0, TCP_SYN, NULL, 0);
    ktest_assert(connection->state == TCP_SYN_RECEIVED, "simultaneous active open");
    expect_output(TCP_SYN | TCP_ACK, connection->iss, 101);
    establish(connection);
    inject(connection, 101, connection->snd_nxt, TCP_RST, NULL, 0);
    file_put(file);
}

static struct tcp_connection *child(uint16_t peer_port)
{
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        if (tcp_connections[i].used && tcp_connections[i].peer_port == peer_port)
            return &tcp_connections[i];
    }
    return NULL;
}
static void passive_checks(void)
{
    struct file *listener_file = new_socket();
    struct socket *listener = socket_from_file(listener_file);
    struct sockaddr_storage name;
    address(&name, LOCAL, 8000);
    ktest_assert(socket_bind(listener, &name, 16) == 0 && socket_listen(listener, 1) == 0,
                 "bind and listen");
    struct file *accepted = NULL;
    ktest_assert(socket_accept(listener, O_NONBLOCK, &accepted, NULL, NULL) == -EAGAIN,
                 "empty accept queue");
    ipv4_input(&fake, segment(9100, 8000, 200, 0, TCP_SYN, NULL, 0));
    struct tcp_connection *first = child(9100);
    ktest_assert(first && first->state == TCP_SYN_RECEIVED && !first->accept_ready,
                 "SYN creates half-open child only");
    ktest_assert(!(listener->ops->poll(listener) & POLLIN), "half-open is not accept readiness");
    inject(first, 200, 0, TCP_SYN, NULL, 0);
    expect_output(TCP_SYN | TCP_ACK, first->iss, 201);
    expire_next(first);
    expect_output(TCP_SYN | TCP_ACK, first->iss, 201);
    ipv4_input(&fake, segment(9101, 8000, 300, 0, TCP_SYN, NULL, 0));
    ktest_assert(!child(9101), "half-open queue is bounded");
    inject(first, 201, first->snd_nxt, TCP_ACK, NULL, 0);
    ktest_assert(first->accept_ready && (listener->ops->poll(listener) & POLLIN),
                 "final ACK promotes to accept queue");
    ipv4_input(&fake, segment(9101, 8000, 300, 0, TCP_SYN, NULL, 0));
    struct tcp_connection *second = child(9101);
    ktest_assert(second, "half-open capacity independent of accept queue");
    inject(second, 301, second->snd_nxt, TCP_ACK, NULL, 0);
    ktest_assert(!child(9101) && first->accept_ready, "full accept queue rejects excess child");
    socklen_t name_length = sizeof name;
    ktest_assert(socket_accept(listener, O_NONBLOCK, &accepted, &name, &name_length) == 0 &&
                     name_length == 16 && ntohs(((struct sockaddr_in *)&name)->sin_port) == 9100,
                 "accept returns established child and peer");
    file_put(listener_file);
    ktest_assert(first->used && first->endpoint, "accepted connection outlives listener");
    inject(first, 201, first->snd_nxt, TCP_RST, NULL, 0);
    file_put(accepted);

    listener_file = new_socket();
    listener = socket_from_file(listener_file);
    address(&name, LOCAL, 8001);
    ktest_assert(socket_bind(listener, &name, 16) == 0 && socket_listen(listener, 2) == 0,
                 "second listener");
    ipv4_input(&fake, segment(9200, 8001, 1, 0, TCP_SYN, NULL, 0));
    first = child(9200);
    ktest_assert(first, "unaccepted child created");
    inject(first, 2, first->snd_nxt, TCP_ACK, NULL, 0);
    ipv4_input(&fake, segment(9201, 8001, 1, 0, TCP_SYN, NULL, 0));
    file_put(listener_file);
    ktest_assert(!child(9200) && !child(9201), "listener close releases both queues");
}

static void parser_checks(void)
{
    for (unsigned kind = 0; kind < 6; kind++) {
        struct pbuf *packet = segment(9000, 8000, 10, 0, TCP_SYN, NULL, 0);
        uint8_t *header = packet->data + 20;
        if (kind == 0)
            header[12] = 4 << 4;
        else if (kind == 1)
            header[12] = 15 << 4;
        else if (kind == 2)
            header[21] = 1;
        else if (kind == 3)
            header[21] = 8;
        else if (kind == 4)
            net_put_be16(header + 22, 0);
        header[16] = header[17] = 0;
        net_put_be16(header + 16, tcp_checksum(PEER, LOCAL, header, 24));
        if (kind == 5)
            header[16] ^= 1;
        uint64_t before = tcp_counters.invalid;
        ipv4_input(&fake, packet);
        ktest_assert(tcp_counters.invalid == before + 1, "malformed TCP input %u rejected", kind);
    }
}

static void close_checks(void)
{
    struct file *file = active_open();
    struct tcp_connection *connection = connection_of(file);
    struct socket *socket = socket_from_file(file);
    establish(connection);
    inject(connection, 101, connection->snd_nxt, TCP_ACK | TCP_FIN, "tail", 4);
    ktest_assert(connection->state == TCP_CLOSE_WAIT && connection->rcv_nxt == 106,
                 "data plus FIN advances receive sequence");
    char buffer[8];
    struct socket_msg message = {
        .data = buffer,
        .len = sizeof buffer,
        .flags = MSG_PEEK,
    };
    ktest_assert(socket_recvmsg(socket, &message) == 4 && !memcmp(buffer, "tail", 4),
                 "peek data preceding EOF");
    message.flags = 0;
    ktest_assert(socket_recvmsg(socket, &message) == 4 && socket_recvmsg(socket, &message) == 0,
                 "queued data delivered before EOF");
    message.data = "reply";
    message.len = 5;
    message.flags = MSG_NOSIGNAL;
    ktest_assert(socket_sendmsg(socket, &message) == 5, "half-close retains write direction");
    ktest_assert(socket_shutdown(socket, SHUT_WR) == 0 && !connection->fin_sent,
                 "FIN waits for outstanding data acknowledgement");
    inject(connection, 106, connection->snd_nxt, TCP_ACK, NULL, 0);
    ktest_assert(connection->state == TCP_LAST_ACK, "peer-first shutdown enters LAST_ACK");
    uint32_t fin_sequence = connection->fin_sequence;
    expire_next(connection);
    expect_output(TCP_FIN | TCP_ACK, fin_sequence, 106);
    inject(connection, 106, connection->snd_nxt, TCP_ACK, NULL, 0);
    ktest_assert(connection->state == TCP_CLOSED, "final ACK closes passive side");
    file_put(file);

    file = active_open();
    connection = connection_of(file);
    establish(connection);
    inject(connection, 101, connection->snd_nxt, TCP_ACK, "unread", 6);
    file_put(file);
    ktest_assert(!connection->used, "unread data close aborts and releases connection");
    ktest_assert(last_frame[47] & TCP_RST, "unread close sends reset");

    file = active_open();
    connection = connection_of(file);
    establish(connection);
    ktest_assert(socket_shutdown(socket_from_file(file), SHUT_WR) == 0, "active half-close");
    inject(connection, 101, connection->fin_sequence, TCP_FIN | TCP_ACK, NULL, 0);
    ktest_assert(connection->state == TCP_CLOSING, "crossing FIN enters CLOSING");
    inject(connection, 102, connection->snd_nxt, TCP_ACK, NULL, 0);
    ktest_assert(connection->state == TCP_TIME_WAIT, "simultaneous close enters TIME_WAIT");
    uint16_t reserved_port = connection->local_port;
    file_put(file);
    ktest_assert(connection->used && !connection->endpoint, "TIME_WAIT outlives socket file");
    struct file *replacement = new_socket();
    struct sockaddr_storage name;
    address(&name, LOCAL, reserved_port);
    ktest_assert(socket_bind(socket_from_file(replacement), &name, 16) == -EADDRINUSE,
                 "TIME_WAIT reserves port");
    uint64_t deadline = connection->lifetime_deadline;
    net_clock_advance(1000);
    inject(connection, 101, connection->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
    ktest_assert(connection->lifetime_deadline > deadline, "duplicate FIN restarts TIME_WAIT");
    inject(connection, 102, connection->snd_nxt, TCP_RST, NULL, 0);
    ktest_assert(connection->state == TCP_TIME_WAIT, "RST cannot remove TIME_WAIT");
    expire_next(connection);
    ktest_assert(!connection->used, "TIME_WAIT expires");
    ktest_assert(socket_bind(socket_from_file(replacement), &name, 16) == 0,
                 "port reusable after TIME_WAIT");
    file_put(replacement);

    file = active_open();
    connection = connection_of(file);
    establish(connection);
    inject(connection, 101, connection->snd_nxt, TCP_ACK, "discard", 7);
    ktest_assert(socket_shutdown(socket_from_file(file), SHUT_RD) == 0 &&
                     !connection->receive_count,
                 "read shutdown discards queued bytes");
    expect_output(TCP_ACK, connection->snd_nxt, 108);
    message.data = buffer;
    message.len = sizeof buffer;
    message.flags = 0;
    ktest_assert(socket_recvmsg(socket_from_file(file), &message) == 0,
                 "read shutdown returns EOF immediately");
    file_put(file);
    inject(connection, 108, connection->snd_nxt, TCP_ACK, "late", 4);
    ktest_assert(!connection->used && (last_frame[47] & TCP_RST),
                 "data arriving after final close aborts orphan");

    file = active_open();
    connection = connection_of(file);
    establish(connection);
    file_put(file);
    inject(connection, 101, connection->snd_nxt, TCP_ACK, NULL, 0);
    ktest_assert(connection->state == TCP_FIN_WAIT_2, "orphan waits for peer FIN");
    expire_next(connection);
    ktest_assert(!connection->used, "orphan FIN_WAIT_2 has bounded lifetime");
}

static int controlled_checks(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "register capture interface");
    netif_set_up(&fake, true);
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0) == 0, "configure capture interface");
    net_clock_control(true);
    tcp_set_generators(initial_sequence, next_port);
    active_checks();
    passive_checks();
    parser_checks();
    close_checks();
    tcp_set_generators(NULL, NULL);
    struct tcp_stats stats;
    ktest_assert(tcp_get_stats(&stats) == 0, "TCP snapshot request completed");
    ktest_assert(!stats.connections && !stats.endpoints, "all TCP objects released");
    net_clock_control(false);
    return 0;
}
static void test_tcp(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    struct net_request request;
    net_request_init(&request, controlled_checks);
    ktest_assert(net_request_run(&request) == 0, "controlled TCP checks");
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(
        before.free == after.free, "TCP packet pool restored %u/%u", before.free, after.free);
    kprintf("net_tcp: ok\n");
}
KTEST_DEFINE("net_tcp", test_tcp);

/* This separate boot case advances the clock from outside netd. It verifies
 * timer dispatch and detached lifetime, without calling a protocol timer. */
static int arm_orphan(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "timer capture interface");
    netif_set_up(&fake, true);
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0) == 0, "timer static address");
    struct file *file = active_open();
    struct tcp_connection *connection = connection_of(file);
    establish(connection);
    file_put(file);
    inject(connection, 101, connection->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
    ktest_assert(connection->used && !connection->endpoint && connection->state == TCP_TIME_WAIT,
                 "detached TIME_WAIT scheduled on netd");
    return 0;
}
static void test_tcp_timer(void)
{
    net_clock_control(true);
    struct net_request request;
    net_request_init(&request, arm_orphan);
    ktest_assert(net_request_run(&request) == 0, "arm closing connection");
    struct tcp_stats stats;
    ktest_assert(tcp_get_stats(&stats) == 0, "TCP snapshot request completed");
    ktest_assert(stats.connections == 1 && stats.time_wait == 1 && !stats.endpoints,
                 "TIME_WAIT retains only protocol state");
    net_clock_advance(TCP_TIME_WAIT_MS - 1);
    net_worker_kick();
    net_worker_drain();
    ktest_assert(tcp_get_stats(&stats) == 0, "TCP snapshot request completed");
    ktest_assert(stats.time_wait == 1, "TIME_WAIT retained before deadline");
    net_clock_advance(1);
    net_worker_kick();
    net_worker_drain();
    ktest_assert(tcp_get_stats(&stats) == 0, "TCP snapshot request completed");
    ktest_assert(!stats.connections && !stats.endpoints, "netd expires TIME_WAIT at deadline");
    net_clock_control(false);
    kprintf("net_tcp_timer: ok\n");
}
KTEST_DEFINE("net_tcp_timer", test_tcp_timer);

static void transfer_checks(void)
{
    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    struct socket *socket = socket_from_file(file);
    establish(c);
    static char data[TCP_SEND_CAPACITY + 37];
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = (char)(i * 37);
    struct socket_msg message = {
        .data = data,
        .len = sizeof data,
        .flags = MSG_NOSIGNAL,
    };
    uint32_t first = c->snd_una;
    ktest_assert(socket_sendmsg(socket, &message) == TCP_SEND_CAPACITY,
                 "partial write fills bounded send buffer");
    ktest_assert(c->transmit_sent == 1200 && !(socket->ops->poll(socket) & POLLOUT),
                 "initial congestion window distinct from send capacity");
    ktest_assert(socket_sendmsg(socket, &message) == -EAGAIN, "full send queue is nonblocking");
    net_clock_advance(20);
    inject(c, 101, first + 600, TCP_ACK, NULL, 0);
    ktest_assert(c->snd_una == first + 600 && c->transmit_length == TCP_SEND_CAPACITY - 600 &&
                     !memcmp(c->transmit, data + 600, c->transmit_length),
                 "partial ACK retains exact unacknowledged suffix across wrap");
    inject(c, 101, first + 1200, TCP_ACK, NULL, 0);
    ktest_assert(c->srtt_ms && c->rto_ms >= 1000 && c->congestion_window > 1200,
                 "unambiguous RTT sample and slow start");
    unsigned window = c->congestion_window;
    for (unsigned duplicate = 0; duplicate < 3; duplicate++)
        inject(c, 101, c->snd_una, TCP_ACK, NULL, 0);
    ktest_assert(c->recovering && c->congestion_window == 1200 && c->slow_start_threshold >= 2400 &&
                     c->congestion_window < window,
                 "duplicate ACKs trigger conservative fast retransmit");
    ktest_assert(!c->sampling, "retransmission suppresses RTT measurement");
    expire_next(c);
    ktest_assert(c->rto_ms == 2000 && c->data_retries == 1,
                 "timeout backs off and retains outstanding bytes");
    while (c->transmit_length)
        inject(c, 101, c->snd_nxt, TCP_ACK, NULL, 0);
    ktest_assert(c->snd_una == first + TCP_SEND_CAPACITY && !c->data_deadline,
                 "all bytes acknowledged without sequence loss");

    /* A lost window-opening ACK is recovered through a bounded probe. */
    struct pbuf *zero = segment(c->peer_port, c->local_port, 101, c->snd_una, TCP_ACK, NULL, 0);
    net_put_be16(zero->data + 34, 0);
    net_put_be16(zero->data + 36, 0);
    net_put_be16(zero->data + 36, tcp_checksum(PEER, LOCAL, zero->data + 20, 20));
    ipv4_input(&fake, zero);
    message.len = 37;
    ktest_assert(socket_sendmsg(socket, &message) == 37 && !c->transmit_sent,
                 "zero window retains queued data");
    expire_next(c);
    expect_output(TCP_ACK, c->snd_una - 1, 101);
    inject(c, 101, c->snd_una, TCP_ACK, NULL, 0);
    ktest_assert(c->transmit_sent == 37, "window refresh resumes sending");
    inject(c, 101, c->snd_nxt, TCP_ACK, NULL, 0);

    /* Reverse order and conflicting duplicate bytes use first-arrival wins.
     * FIN is retained until the gap before it has been filled. */
    inject(c, 105, c->snd_nxt, TCP_ACK | TCP_FIN, "efgh", 4);
    ktest_assert(c->out_of_order == 4 && c->rcv_nxt == 101 && !c->peer_fin,
                 "out-of-order data and FIN wait for gap");
    inject(c, 105, c->snd_nxt, TCP_ACK, "XXXX", 4);
    inject(c, 101, c->snd_nxt, TCP_ACK, "abcdef", 6);
    char received[16];
    message.data = received;
    message.len = sizeof received;
    message.flags = 0;
    ktest_assert(socket_recvmsg(socket, &message) == 8 && !memcmp(received, "abcdefgh", 8),
                 "overlap and reorder preserve exact stream");
    ktest_assert(socket_recvmsg(socket, &message) == 0 && c->rcv_nxt == 110,
                 "reordered FIN follows all data");
    file_put(file);
    inject(c, 110, c->snd_nxt, TCP_ACK, NULL, 0);
    ktest_assert(!c->used, "transfer connection released");
}
static int controlled_transfer(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "transfer capture interface");
    netif_set_up(&fake, true);
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0) == 0, "transfer address");
    net_clock_control(true);
    tcp_set_generators(initial_sequence, next_port);
    transfer_checks();
    tcp_set_generators(NULL, NULL);
    net_clock_control(false);
    return 0;
}
static void test_tcp_transfer(void)
{
    struct net_request request;
    net_request_init(&request, controlled_transfer);
    ktest_assert(net_request_run(&request) == 0, "controlled transfer completed");
    kprintf("net_tcp_transfer: ok\n");
}
KTEST_DEFINE("net_tcp_transfer", test_tcp_transfer);

static int path_checks(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "path capture interface");
    netif_set_up(&fake, true);
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0) == 0, "path address");
    net_clock_control(true);
    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    establish(c);
    static char payload[1000];
    struct socket_msg message = {
        .data = payload,
        .len = sizeof payload,
        .flags = MSG_NOSIGNAL,
    };
    ktest_assert(socket_sendmsg(socket_from_file(file), &message) == sizeof payload,
                 "packet retained for PMTU recovery");
    uint8_t quote[28];
    memcpy(quote, last_frame + 14, sizeof quote);
    quote[24] ^= 1;
    ipv4_path_feedback(quote, sizeof quote, 600);
    ktest_assert(ipv4_path_mtu(PEER, 1500) == 1500 && net_ip_stats.pmtu_rejected == 1,
                 "forged TCP sequence cannot change path MTU");
    quote[24] ^= 1;
    ipv4_path_feedback(quote, sizeof quote, 600);
    ktest_assert(ipv4_path_mtu(PEER, 1500) == 600 && c->peer_mss == 560,
                 "matching recent transmission lowers segment size");
    expire_next(c);
    ktest_assert(net_get_be16(last_frame + 16) == 600, "retained data retransmitted at new MTU");
    while (c->transmit_length)
        inject(c, c->rcv_nxt, c->snd_nxt, TCP_ACK, NULL, 0);
    net_clock_advance(600001);
    ktest_assert(ipv4_path_mtu(PEER, 1500) == 1500, "path cache expires");
    inject(c, c->rcv_nxt, c->snd_nxt, TCP_RST, NULL, 0);
    file_put(file);

    file = active_open();
    c = connection_of(file);
    establish(c);
    ktest_assert(socket_sendmsg(socket_from_file(file), &message) == sizeof payload,
                 "black-hole packet queued");
    expire_next(c);
    expire_next(c);
    ktest_assert(c->peer_mss == 536, "missing ICMP triggers bounded smaller-segment fallback");
    while (c->state != TCP_CLOSED)
        expire_next(c);
    expect_error(socket_from_file(file), ETIMEDOUT);
    file_put(file);
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0) == 0, "route replacement");
    ktest_assert(ipv4_path_mtu(PEER, 1500) == 1500 && !ipv4_validate_quote(quote, sizeof quote),
                 "route replacement invalidates path and correlation cache");
    net_clock_control(false);
    return 0;
}
static void test_path(void)
{
    struct net_request request;
    net_request_init(&request, path_checks);
    ktest_assert(net_request_run(&request) == 0, "path checks completed");
    kprintf("net_path: ok\n");
}
KTEST_DEFINE("net_path", test_path);

static int pressure_prepare(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "pressure interface");
    netif_set_up(&fake, true);
    return net_configure(&fake, LOCAL, 0xffffff00u, 0);
}
static int pressure_cycle(struct net_request *request)
{
    struct file *files[TCP_ENDPOINTS];
    for (unsigned i = 0; i < TCP_ENDPOINTS; i++)
        files[i] = new_socket();
    struct file *extra;
    ktest_assert(socket_create(AF_INET, SOCK_STREAM, 0, O_NONBLOCK, &extra) == -ENOBUFS,
                 "endpoint table exhaustion is explicit");
    for (unsigned i = 0; i < TCP_ENDPOINTS; i++)
        file_put(files[i]);

    struct file *listener_file = new_socket();
    struct socket *listener = socket_from_file(listener_file);
    struct sockaddr_storage name;
    address(&name, LOCAL, 8000);
    ktest_assert(socket_bind(listener, &name, 16) == 0 && socket_listen(listener, 16) == 0,
                 "pressure listener");
    for (unsigned i = 0; i < 1000; i++)
        ipv4_input(&fake, segment(10000 + i, 8000, i, 0, TCP_SYN, NULL, 0));
    struct tcp_stats stats;
    ktest_assert(tcp_get_stats(&stats) == 0 && stats.half_open == TCP_SYN_BACKLOG,
                 "SYN pressure leaves bounded half-open state");
    file_put(listener_file);
    resolve_peer(); /* Release queued SYN ACKs after the bounded ARP wait. */

    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    establish(c);
    /* The store is larger than one packet since N13, so the peer fills the
     * offered window in 8000-byte segments until it is closed. */
    static char data[8000];
    for (unsigned window; (window = tcp_receive_window(c));)
        inject(c, c->rcv_nxt, c->snd_nxt, TCP_ACK, data, MIN(window, sizeof data));
    ktest_assert(c->receive_count == TCP_RECEIVE_CAPACITY && !tcp_receive_window(c),
                 "slow reader fills only its bounded storage");
    struct file *other = active_open();
    struct tcp_connection *independent = connection_of(other);
    establish(independent);
    struct socket_msg message = {
        .data = data,
        .len = 16,
        .flags = MSG_NOSIGNAL,
    };
    ktest_assert(socket_sendmsg(socket_from_file(other), &message) == 16,
                 "unrelated connection progresses beside slow reader");
    inject(independent, independent->rcv_nxt, independent->snd_nxt, TCP_ACK, NULL, 0);
    inject(independent, independent->rcv_nxt, independent->snd_nxt, TCP_RST, NULL, 0);
    file_put(other);
    file_put(file);

    /* Data allocations cannot consume the control reserve. A SYN still
     * traverses production output while every data buffer is allocated. */
    struct pbuf *bufs[NET_PBUF_COUNT];
    unsigned nbufs = 0;
    while ((bufs[nbufs] = pbuf_alloc(PBUF_DATA)))
        nbufs++;
    ktest_assert(nbufs == NET_PBUF_COUNT - NET_PBUF_RESERVE, "data floor enforced");
    file = active_open();
    c = connection_of(file);
    expect_output(TCP_SYN, c->iss, 0);
    file_put(file);
    for (unsigned i = 0; i < nbufs; i++)
        pbuf_free(bufs[i]);

    file = active_open();
    c = connection_of(file);
    establish(c);
    ktest_assert(netif_set_up(&fake, false) == 0, "interface down under established traffic");
    expect_error(socket_from_file(file), ENETDOWN);
    file_put(file);
    netif_set_up(&fake, true);

    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        file = active_open();
        c = connection_of(file);
        establish(c);
        file_put(file);
        inject(c, c->rcv_nxt, c->snd_nxt, TCP_ACK | TCP_FIN, NULL, 0);
        ktest_assert(c->state == TCP_TIME_WAIT, "TIME_WAIT pressure %u", i);
    }
    ktest_assert(tcp_get_stats(&stats) == 0 && stats.time_wait == TCP_CONNECTIONS &&
                     !stats.endpoints,
                 "TIME_WAIT table full without retaining socket files");
    file = new_socket();
    address(&name, PEER, 9000);
    ktest_assert(socket_connect(socket_from_file(file), &name, 16) == -ENOBUFS,
                 "connection-table pressure fails explicitly");
    file_put(file);
    return 0;
}
static void test_pressure(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    net_clock_control(true);
    struct net_request request;
    net_request_init(&request, pressure_prepare);
    ktest_assert(net_request_run(&request) == 0, "prepare pressure test");
    for (unsigned cycle = 0; cycle < 3; cycle++) {
        net_request_init(&request, pressure_cycle);
        ktest_assert(net_request_run(&request) == 0, "pressure cycle %u", cycle);
        net_clock_advance(TCP_TIME_WAIT_MS);
        net_worker_kick();
        net_worker_drain();
        struct tcp_stats stats;
        ktest_assert(tcp_get_stats(&stats) == 0 && !stats.connections && !stats.endpoints,
                     "pressure cycle %u returns to empty TCP tables",
                     cycle);
        pbuf_get_stats(&after);
        ktest_assert(after.free == before.free, "pressure cycle %u packet recovery", cycle);
    }
    net_clock_control(false);
    kprintf("net_pressure: 3 cycles, TCP 64/64, packet low-water %u, recovered\n", after.low_water);
}
KTEST_DEFINE("net_pressure", test_pressure);
