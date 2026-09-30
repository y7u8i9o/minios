/* TCP options with injected segments and the controlled clock. A fake
 * Ethernet device records production output, injected segments carry
 * options built here, and the captured options are decoded by this file
 * rather than by the production parser, so the two cannot agree by
 * sharing a mistake. */
#include <console.h>
#include <errno.h>
#include "net_helpers.h"
#include "../net/tcp/internal.h"

#define LOCAL 0x0a000002u
#define PEER 0x0a000001u
#define FRAMES 64

static uint8_t frames[FRAMES][1600];
static size_t frame_lengths[FRAMES];
static unsigned frame_count;
static const uint8_t peer_mac[6] = {2, 0, 0, 0, 0, 1};

static int capture(struct netif *interface, struct pbuf *packet)
{
    unsigned slot = frame_count++ % FRAMES;
    frame_lengths[slot] = MIN(packet->len, sizeof frames[slot]);
    memcpy(frames[slot], packet->data, frame_lengths[slot]);
    pbuf_free(packet);
    return 0;
}
static const struct netif_ops fake_ops = {
    .output = capture,
    .input = ethernet_input,
};
static struct netif fake = {
    .name = "tcpopt",
    .flags = NETIF_ETHERNET,
    .mtu = 1500,
    .hwaddr = {2, 0, 0, 0, 0, 2},
    .ops = &fake_ops,
};

static uint32_t initial_sequence(void)
{
    return 0x7ffffff0u;
}
static uint16_t next_port(void)
{
    static uint16_t port = 51000;
    return port++;
}

static void resolve_peer(void)
{
    struct pbuf *packet = pbuf_alloc(PBUF_CONTROL);
    ktest_assert(packet, "ARP packet allocation");
    uint8_t *h = pbuf_put(packet, 28);
    memset(h, 0, 28);
    net_put_be16(h, 1);
    net_put_be16(h + 2, 0x0800);
    h[4] = 6;
    h[5] = 4;
    net_put_be16(h + 6, 2);
    memcpy(h + 8, peer_mac, 6);
    net_put_be32(h + 14, PEER);
    memcpy(h + 18, fake.hwaddr, 6);
    net_put_be32(h + 24, LOCAL);
    arp_input(&fake, packet);
}

/* Options of an injected segment. */
struct peer_options {
    uint16_t mss;
    int window_scale; /* negative: absent */
    bool timestamp;
    uint32_t ts_value, ts_echo;
    int bad_length; /* nonzero: that length for the first option emitted */
};

static size_t build_options(uint8_t *o, const struct peer_options *p)
{
    size_t n = 0;
    if (p->mss) {
        o[n] = 2;
        o[n + 1] = 4;
        net_put_be16(o + n + 2, p->mss);
        n += 4;
    }
    /* A bad length keeps the option well framed, so the parser has to
     * reject it for its length rather than for overrunning the header. */
    if (p->window_scale >= 0 && p->bad_length) {
        uint8_t bad[4] = {3, 4, (uint8_t)p->window_scale, 0};
        memcpy(o + n, bad, 4);
        n += 4;
    } else if (p->window_scale >= 0) {
        uint8_t scale[4] = {1, 3, 3, (uint8_t)p->window_scale};
        memcpy(o + n, scale, 4);
        n += 4;
    }
    if (p->timestamp && p->bad_length && p->window_scale < 0) {
        uint8_t bad[12] = {8, 8, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1};
        memcpy(o + n, bad, 12);
        n += 12;
    } else if (p->timestamp) {
        o[n] = 1;
        o[n + 1] = 1;
        o[n + 2] = 8;
        o[n + 3] = 10;
        net_put_be32(o + n + 4, p->ts_value);
        net_put_be32(o + n + 8, p->ts_echo);
        n += 12;
    }
    return n;
}

static void send_segment(uint16_t source_port,
                         uint16_t destination_port,
                         uint32_t sequence,
                         uint32_t acknowledgement,
                         uint8_t flags,
                         uint16_t window,
                         const struct peer_options *options,
                         const void *data,
                         size_t length)
{
    uint8_t option_bytes[40];
    size_t option_length = build_options(option_bytes, options);
    size_t header_length = 20 + option_length;
    struct pbuf *packet = ip_packet(PEER, LOCAL, IPPROTO_TCP, header_length + length);
    uint8_t *h = packet->data + 20;
    net_put_be16(h, source_port);
    net_put_be16(h + 2, destination_port);
    net_put_be32(h + 4, sequence);
    net_put_be32(h + 8, acknowledgement);
    h[12] = (uint8_t)(header_length / 4) << 4;
    h[13] = flags;
    net_put_be16(h + 14, window);
    memcpy(h + 20, option_bytes, option_length);
    if (length)
        memcpy(h + header_length, data, length);
    net_put_be16(h + 16, tcp_checksum(PEER, LOCAL, h, header_length + length));
    ipv4_input(&fake, packet);
}

static void inject(struct tcp_connection *c,
                   uint32_t sequence,
                   uint8_t flags,
                   uint16_t window,
                   const struct peer_options *options,
                   const void *data,
                   size_t length)
{
    /* A SYN ACK acknowledges our SYN; other injected segments acknowledge
     * nothing new, so outstanding data stays outstanding. */
    uint32_t acknowledgement = flags & TCP_SYN ? c->snd_nxt : c->snd_una;
    send_segment(c->peer_port, c->local_port, sequence, acknowledgement, flags, window, options,
                 data, length);
}

/* One captured TCP segment decoded independently of wire.c. */
struct decoded {
    uint8_t flags;
    uint32_t sequence, acknowledgement;
    uint16_t window;
    size_t header_length, payload;
    bool mss, window_scale, timestamp;
    uint16_t mss_value;
    uint8_t shift;
    uint32_t ts_value, ts_echo;
};

static struct decoded decode_frame(unsigned index)
{
    const uint8_t *f = frames[index % FRAMES];
    ktest_assert(frame_lengths[index % FRAMES] >= 54 && net_get_be16(f + 12) == 0x0800 &&
                     f[23] == IPPROTO_TCP,
                 "captured frame %u is TCP",
                 index);
    const uint8_t *h = f + 34;
    size_t total = net_get_be16(f + 16) - 20;
    ktest_assert(tcp_checksum(LOCAL, PEER, h, total) == 0, "output checksum of frame %u", index);
    struct decoded d = {
        .flags = h[13],
        .sequence = net_get_be32(h + 4),
        .acknowledgement = net_get_be32(h + 8),
        .window = net_get_be16(h + 14),
        .header_length = (size_t)(h[12] >> 4) * 4,
    };
    d.payload = total - d.header_length;
    for (size_t o = 20; o < d.header_length;) {
        if (h[o] == 0)
            break;
        if (h[o] == 1) {
            o++;
            continue;
        }
        ktest_assert(o + 2 <= d.header_length && h[o + 1] >= 2 &&
                         o + h[o + 1] <= d.header_length,
                     "output option framing");
        if (h[o] == 2 && h[o + 1] == 4) {
            d.mss = true;
            d.mss_value = net_get_be16(h + o + 2);
        } else if (h[o] == 3 && h[o + 1] == 3) {
            d.window_scale = true;
            d.shift = h[o + 2];
        } else if (h[o] == 8 && h[o + 1] == 10) {
            d.timestamp = true;
            d.ts_value = net_get_be32(h + o + 2);
            d.ts_echo = net_get_be32(h + o + 6);
        } else {
            ktest_assert(false, "unexpected output option kind %u", h[o]);
        }
        o += h[o + 1];
    }
    return d;
}
static struct decoded last(void)
{
    return decode_frame(frame_count - 1);
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
static long send_bytes(struct file *file, const void *data, size_t length)
{
    struct socket_msg message = {
        .data = (void *)data,
        .len = length,
        .flags = MSG_NOSIGNAL,
    };
    return socket_sendmsg(socket_from_file(file), &message);
}
static void expire_next(struct tcp_connection *c)
{
    ktest_assert(net_timer_armed(&c->timer), "connection timer armed");
    net_clock_set(c->timer.deadline_ms);
    net_timer_cancel(&c->timer);
    tcp_timer_expire(&c->timer);
}
static void reset(struct tcp_connection *c)
{
    struct peer_options none = {.window_scale = -1};
    inject(c, c->rcv_nxt, TCP_RST, 0, &none, NULL, 0);
}

static char pattern[4096];

/* Our SYN offers both options; a SYN ACK without them leaves the
 * connection unscaled and without timestamps (N13 fallback). */
static void fallback_checks(void)
{
    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    struct decoded syn = last();
    ktest_assert(syn.flags == TCP_SYN && syn.header_length == 40 && syn.mss &&
                     syn.mss_value == 1460 && syn.window_scale && syn.shift == TCP_WINDOW_SHIFT &&
                     syn.timestamp && !syn.ts_echo && syn.window == 65535,
                 "SYN offers MSS, window scale %u and a timestamp",
                 syn.shift);
    struct peer_options plain = {.mss = 1200, .window_scale = -1};
    inject(c, 100, TCP_SYN | TCP_ACK, 4096, &plain, NULL, 0);
    ktest_assert(c->state == TCP_ESTABLISHED && !c->window_scaling && !c->timestamps &&
                     !c->snd_scale && !c->rcv_scale,
                 "SYN ACK without options disables both");
    struct decoded ack = last();
    ktest_assert(ack.flags == TCP_ACK && ack.header_length == 20 && ack.window == 65535,
                 "unscaled ACK carries no option and a capped window");
    ktest_assert(send_bytes(file, pattern, 3000) == 3000, "fallback send");
    struct decoded data = last();
    ktest_assert(data.header_length == 20 && data.payload == 1200,
                 "fallback segments use the full MSS without options");
    reset(c);
    file_put(file);
}

static void negotiated_checks(void)
{
    uint64_t samples = tcp_counters.timestamp_samples;
    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    struct decoded syn = last();
    struct peer_options options = {
        .mss = 1200,
        .window_scale = 7,
        .timestamp = true,
        .ts_value = 5000,
        .ts_echo = syn.ts_value,
    };
    inject(c, 100, TCP_SYN | TCP_ACK, 1, &options, NULL, 0);
    ktest_assert(c->state == TCP_ESTABLISHED && c->window_scaling && c->snd_scale == 7 &&
                     c->rcv_scale == TCP_WINDOW_SHIFT && c->timestamps &&
                     c->ts_recent == 5000 && c->peer_window == 1,
                 "SYN ACK options negotiate scaling and timestamps; its window is unscaled");
    struct decoded ack = last();
    ktest_assert(ack.flags == TCP_ACK && ack.header_length == 32 && ack.timestamp &&
                     ack.ts_echo == 5000 && ack.window == TCP_RECEIVE_CAPACITY >> TCP_WINDOW_SHIFT,
                 "ACK echoes the peer timestamp and advertises a scaled window of %u",
                 ack.window);

    /* A scaled window of one unit is 128 bytes, not one byte. */
    options = (struct peer_options){.window_scale = -1, .timestamp = true, .ts_value = 5001};
    inject(c, 101, TCP_ACK, 1, &options, NULL, 0);
    ktest_assert(c->peer_window == 128, "peer window shifted by its scale");
    ktest_assert(send_bytes(file, pattern, 3000) == 3000, "scaled send");
    struct decoded first = last();
    ktest_assert(c->transmit_sent == 128 && first.payload == 128 && first.timestamp &&
                     first.ts_echo == 5001,
                 "scaled window limits the flight to %u bytes",
                 (unsigned)c->transmit_sent);

    /* The echoed timestamp gives a round-trip sample of 40 ms. */
    net_clock_advance(40);
    options.ts_value = 5002;
    options.ts_echo = first.ts_value;
    send_segment(c->peer_port, c->local_port, 101, c->snd_una + 128, TCP_ACK, 100, &options,
                 NULL, 0);
    ktest_assert(c->srtt_ms == 40 && tcp_counters.timestamp_samples == samples + 1,
                 "timestamp RTT sample %u ms",
                 c->srtt_ms);
    ktest_assert(c->peer_window == 12800, "window update is scaled");
    unsigned before = frame_count;
    struct decoded full = decode_frame(before - 2);
    ktest_assert(full.payload == 1200 - TCP_TIMESTAMP_SPACE && full.header_length == 32,
                 "a full segment leaves room for the timestamp option (payload %u)",
                 (unsigned)full.payload);

    /* PAWS: an older timestamp is dropped and answered; a segment without
     * one is dropped silently; a newer one is accepted. */
    uint32_t expected = c->rcv_nxt;
    uint64_t paws = tcp_counters.paws_rejected, missing = tcp_counters.timestamp_missing;
    options.ts_value = 4000;
    options.ts_echo = 0;
    inject(c, expected, TCP_ACK, 100, &options, "old", 3);
    ktest_assert(c->rcv_nxt == expected && tcp_counters.paws_rejected == paws + 1 &&
                     frame_count == before + 1 && last().acknowledgement == expected,
                 "PAWS drops an old duplicate and acknowledges");
    struct peer_options bare = {.window_scale = -1};
    inject(c, expected, TCP_ACK, 100, &bare, "bare", 4);
    ktest_assert(c->rcv_nxt == expected && tcp_counters.timestamp_missing == missing + 1 &&
                     frame_count == before + 1,
                 "segment without a timestamp dropped silently");
    options.ts_value = 5003;
    inject(c, expected, TCP_ACK, 100, &options, "new", 3);
    ktest_assert(c->rcv_nxt == expected + 3 && c->ts_recent == 5003 && last().ts_echo == 5003,
                 "newer timestamp accepted and echoed");

    /* Out-of-order data beyond the last ACK does not move TS.Recent. */
    options.ts_value = 6000;
    inject(c, expected + 13, TCP_ACK, 100, &options, "later", 5);
    ktest_assert(c->ts_recent == 5003 && last().ts_echo == 5003,
                 "segment beyond Last.ACK.sent leaves TS.Recent");
    options.ts_value = 5004;
    inject(c, expected + 3, TCP_ACK, 100, &options, "0123456789", 10);
    ktest_assert(c->rcv_nxt == expected + 18 && c->ts_recent == 5004,
                 "hole filled with the older timestamp");

    /* A retransmission is measured through its own timestamp. */
    uint32_t una = c->snd_una;
    expire_next(c);
    struct decoded retransmission = last();
    ktest_assert(retransmission.sequence == una && retransmission.timestamp,
                 "retransmission carries a fresh timestamp");
    net_clock_advance(60);
    options.ts_value = 5005;
    options.ts_echo = retransmission.ts_value;
    send_segment(c->peer_port, c->local_port, c->rcv_nxt, c->snd_nxt, TCP_ACK, 100, &options,
                 NULL, 0);
    ktest_assert(tcp_counters.timestamp_samples == samples + 2 && c->srtt_ms > 40,
                 "sample taken after a retransmission (srtt %u)",
                 c->srtt_ms);

    /* A reset needs no timestamp. */
    inject(c, c->rcv_nxt, TCP_RST, 0, &bare, NULL, 0);
    ktest_assert(c->state == TCP_CLOSED, "reset without timestamp accepted");
    file_put(file);
}

static struct tcp_connection *child(uint16_t peer_port)
{
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++)
        if (tcp_connections[i].used && tcp_connections[i].peer_port == peer_port)
            return &tcp_connections[i];
    return NULL;
}

static void passive_checks(void)
{
    struct file *listener = new_socket();
    struct sockaddr_storage name;
    address(&name, LOCAL, 8100);
    ktest_assert(socket_bind(socket_from_file(listener), &name, 16) == 0 &&
                     socket_listen(socket_from_file(listener), 4) == 0,
                 "options listener");
    struct peer_options options = {.mss = 1400, .window_scale = 3, .timestamp = true, .ts_value = 700};
    send_segment(9300, 8100, 1, 0, TCP_SYN, 65535, &options, NULL, 0);
    struct tcp_connection *c = child(9300);
    struct decoded synack = last();
    ktest_assert(c && c->window_scaling && c->snd_scale == 3 && c->timestamps &&
                     synack.flags == (TCP_SYN | TCP_ACK) && synack.window_scale &&
                     synack.shift == TCP_WINDOW_SHIFT && synack.timestamp &&
                     synack.ts_echo == 700 && synack.window == 65535,
                 "SYN ACK repeats both offered options with an unscaled window");
    options = (struct peer_options){.window_scale = -1, .timestamp = true, .ts_value = 701,
                                    .ts_echo = synack.ts_value};
    send_segment(9300, 8100, 2, c->snd_nxt, TCP_ACK, 10, &options, NULL, 0);
    ktest_assert(c->state == TCP_ESTABLISHED && c->peer_window == 80,
                 "final ACK window scaled by the peer shift");

    struct peer_options plain = {.mss = 1400, .window_scale = -1};
    send_segment(9301, 8100, 1, 0, TCP_SYN, 65535, &plain, NULL, 0);
    synack = last();
    ktest_assert(child(9301) && !child(9301)->window_scaling && !child(9301)->timestamps &&
                     synack.header_length == 24 && synack.mss && !synack.window_scale &&
                     !synack.timestamp,
                 "SYN without options is answered with MSS alone");

    struct peer_options large = {.mss = 1400, .window_scale = 15};
    send_segment(9302, 8100, 1, 0, TCP_SYN, 65535, &large, NULL, 0);
    ktest_assert(child(9302) && child(9302)->snd_scale == TCP_MAX_WINDOW_SHIFT,
                 "shift above 14 is clamped to 14");

    uint64_t invalid = tcp_counters.invalid;
    struct peer_options bad_scale = {.mss = 1400, .window_scale = 2, .bad_length = 4};
    send_segment(9303, 8100, 1, 0, TCP_SYN, 65535, &bad_scale, NULL, 0);
    struct peer_options bad_stamp = {.mss = 1400, .window_scale = -1, .timestamp = true,
                                     .bad_length = 8};
    send_segment(9304, 8100, 1, 0, TCP_SYN, 65535, &bad_stamp, NULL, 0);
    ktest_assert(!child(9303) && !child(9304) && tcp_counters.invalid == invalid + 2,
                 "malformed window scale and timestamp options rejected");
    file_put(listener);
    ktest_assert(!child(9301) && !child(9302), "listener close releases children");
}

/* After 24 idle days TS.Recent no longer rejects an older timestamp. */
static void idle_checks(void)
{
    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    struct peer_options options = {.mss = 1200, .window_scale = 0, .timestamp = true,
                                   .ts_value = 90000, .ts_echo = last().ts_value};
    inject(c, 100, TCP_SYN | TCP_ACK, 4096, &options, NULL, 0);
    ktest_assert(c->timestamps && c->window_scaling && !c->snd_scale, "shift 0 negotiated");
    net_clock_advance(TCP_PAWS_IDLE_MS + 1000);
    resolve_peer(); /* The neighbour entry expired with the idle time. */
    options = (struct peer_options){.window_scale = -1, .timestamp = true, .ts_value = 10};
    inject(c, c->rcv_nxt, TCP_ACK, 4096, &options, "idle", 4);
    ktest_assert(c->rcv_nxt == 105 && c->ts_recent == 10, "PAWS idle limit resets TS.Recent");
    reset(c);
    file_put(file);
}

static int controlled_options(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "register option capture interface");
    netif_set_up(&fake, true);
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0) == 0, "option test address");
    for (unsigned i = 0; i < sizeof pattern; i++)
        pattern[i] = (char)(i * 13);
    net_clock_control(true);
    tcp_set_generators(initial_sequence, next_port);
    fallback_checks();
    negotiated_checks();
    passive_checks();
    idle_checks();
    tcp_set_generators(NULL, NULL);
    struct tcp_stats stats;
    ktest_assert(tcp_get_stats(&stats) == 0 && !stats.connections && !stats.endpoints,
                 "all option test connections released");
    net_clock_control(false);
    netif_set_up(&fake, false);
    return 0;
}

static void test_tcp_options(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    struct net_request request;
    net_request_init(&request, controlled_options);
    ktest_assert(net_request_run(&request) == 0, "controlled option checks");
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(before.free == after.free, "packet pool restored %u/%u", before.free, after.free);
    kprintf("net_tcp_options: window scaling, timestamps, PAWS and RTT, ok\n");
}
KTEST_DEFINE("net_tcp_options", test_tcp_options);
