/* These tests check TCP options with injected segments and the controlled
 * clock. A fake Ethernet device records production output, injected
 * segments carry options built here, and the captured options are decoded
 * by this file rather than by the production parser, so the two cannot
 * agree by sharing a mistake. */
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

/* struct peer_options describes the options of an injected segment. A
 * negative window_scale leaves the option out, and a nonzero bad_length
 * replaces the length of the window scale or timestamp option. */
struct peer_options {
    uint16_t mss;
    int window_scale;
    bool timestamp;
    uint32_t ts_value, ts_echo;
    int bad_length;
    bool sack_permitted;
    unsigned sack_count;
    uint32_t sack[4][2];
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
    if (p->sack_permitted) {
        uint8_t permitted[4] = {1, 1, 4, 2};
        memcpy(o + n, permitted, 4);
        n += 4;
    }
    if (p->sack_count) {
        o[n] = 1;
        o[n + 1] = 1;
        o[n + 2] = 5;
        o[n + 3] = (uint8_t)(2 + 8 * p->sack_count);
        for (unsigned i = 0; i < p->sack_count; i++) {
            net_put_be32(o + n + 4 + 8 * i, p->sack[i][0]);
            net_put_be32(o + n + 8 + 8 * i, p->sack[i][1]);
        }
        n += 4 + 8 * p->sack_count;
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

/* struct decoded holds one captured TCP segment, decoded independently of
 * wire.c. */
struct decoded {
    uint8_t flags;
    uint32_t sequence, acknowledgement;
    uint16_t window;
    size_t header_length, payload;
    bool mss, window_scale, timestamp, sack_permitted;
    uint16_t mss_value;
    uint8_t shift;
    uint32_t ts_value, ts_echo;
    unsigned sack_count;
    uint32_t sack[4][2];
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
        } else if (h[o] == 4 && h[o + 1] == 2) {
            d.sack_permitted = true;
        } else if (h[o] == 5 && h[o + 1] >= 10 && (h[o + 1] - 2) % 8 == 0 && h[o + 1] <= 34) {
            d.sack_count = (h[o + 1] - 2u) / 8;
            for (unsigned b = 0; b < d.sack_count; b++) {
                d.sack[b][0] = net_get_be32(h + o + 2 + 8 * b);
                d.sack[b][1] = net_get_be32(h + o + 6 + 8 * b);
            }
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
                     syn.timestamp && !syn.ts_echo && syn.window == 65535 && syn.sack_permitted,
                 "SYN offers MSS, window scale %u, SACK and a timestamp",
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

    /* PAWS drops an older timestamp and answers it, drops a segment without
     * one silently and accepts a newer one. */
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
    ktest_assert(c->rcv_nxt == expected + 3 && c->ts_recent == 5003 && c->ack_deadline,
                 "newer timestamp accepted, its ACK delayed");
    expire_next(c);
    ktest_assert(last().ts_echo == 5003 && last().acknowledgement == expected + 3,
                 "delayed ACK echoes the newer timestamp");

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
    struct peer_options options = {
        .mss = 1400, .window_scale = 3, .timestamp = true, .ts_value = 700};
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
                     !child(9301)->sack && synack.header_length == 24 && synack.mss &&
                     !synack.window_scale && !synack.timestamp && !synack.sack_permitted,
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

/* The N14 tests check SACK (RFC 2018, RFC 6675) and delayed ACKs. */

/* sack_open returns an established connection that negotiated SACK, with
 * the peer's MSS 1000 and a scaled peer window of 128000 bytes. Timestamps
 * are optional so that both option budgets are exercised. */
static struct file *sack_open(bool timestamps)
{
    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    struct peer_options options = {
        .mss = 1000,
        .window_scale = 7,
        .sack_permitted = true,
        .timestamp = timestamps,
        .ts_value = 1,
        .ts_echo = last().ts_value,
    };
    inject(c, 100, TCP_SYN | TCP_ACK, 1000, &options, NULL, 0);
    ktest_assert(c->state == TCP_ESTABLISHED && c->sack && c->timestamps == timestamps,
                 "SACK negotiated");
    return file;
}
static struct peer_options ack_options(struct tcp_connection *c)
{
    struct peer_options o = {.window_scale = -1, .timestamp = c->timestamps};
    if (c->timestamps) {
        o.ts_value = c->ts_recent + 1;
        o.ts_echo = c->ts_recent ? tcp_timestamp_now(c) : 0;
    }
    return o;
}
/* peer_ack injects a pure ACK from the peer with the given cumulative ACK
 * and SACK blocks. */
static void peer_ack(struct tcp_connection *c,
                     uint32_t ack,
                     unsigned blocks,
                     const uint32_t (*sack)[2])
{
    struct peer_options o = ack_options(c);
    o.sack_count = blocks;
    for (unsigned i = 0; i < blocks; i++) {
        o.sack[i][0] = sack[i][0];
        o.sack[i][1] = sack[i][1];
    }
    send_segment(c->peer_port, c->local_port, c->rcv_nxt, ack, TCP_ACK, 1000, &o, NULL, 0);
}
static void peer_data(struct tcp_connection *c, uint32_t sequence, size_t length)
{
    struct peer_options o = ack_options(c);
    inject(c, sequence, TCP_ACK, 1000, &o, pattern, length);
}

static void receiver_checks(bool timestamps)
{
    struct file *file = sack_open(timestamps);
    struct tcp_connection *c = connection_of(file);
    uint32_t r = c->rcv_nxt;
    unsigned before = frame_count;
    peer_data(c, r + 100, 100);
    struct decoded d = last();
    ktest_assert(frame_count == before + 1 && d.acknowledgement == r && d.sack_count == 1 &&
                     d.sack[0][0] == r + 100 && d.sack[0][1] == r + 200,
                 "out-of-order data acknowledged at once with its SACK block");
    peer_data(c, r + 300, 100);
    d = last();
    ktest_assert(d.sack_count == 2 && d.sack[0][0] == r + 300 && d.sack[1][0] == r + 100,
                 "most recent block first (RFC 2018)");
    peer_data(c, r + 500, 100);
    peer_data(c, r + 700, 100);
    peer_data(c, r + 900, 100);
    d = last();
    unsigned fit = timestamps ? 3 : 4;
    ktest_assert(d.sack_count == fit && d.sack[0][0] == r + 900 && d.sack[1][0] == r + 700 &&
                     d.header_length <= 60,
                 "%u blocks fit beside %s",
                 fit,
                 timestamps ? "the timestamp" : "no timestamp");
    peer_data(c, r + 200, 100);
    d = last();
    ktest_assert(d.sack[0][0] == r + 100 && d.sack[0][1] == r + 400,
                 "filled gap merges reported blocks");
    before = frame_count;
    peer_data(c, r, 100);
    d = last();
    ktest_assert(frame_count == before + 1 && d.acknowledgement == r + 400 &&
                     d.sack_count == 3 && d.sack[0][0] == r + 900 && d.sack[2][0] == r + 500,
                 "hole filled: immediate ACK keeps the remaining blocks in report order");
    peer_data(c, r + 400, 100);
    peer_data(c, r + 600, 100);
    peer_data(c, r + 800, 100);
    d = last();
    ktest_assert(d.acknowledgement == r + 1000 && !d.sack_count,
                 "no block once the stream is contiguous");
    reset(c);
    file_put(file);
}

static void delayed_ack_checks(void)
{
    struct file *file = sack_open(false);
    struct tcp_connection *c = connection_of(file);
    uint64_t delayed = tcp_counters.delayed_acks, timeouts = tcp_counters.delayed_ack_timeouts;
    unsigned before = frame_count;
    uint64_t start = net_clock_ms();
    peer_data(c, c->rcv_nxt, 100);
    ktest_assert(frame_count == before && c->ack_deadline == start + TCP_DELAYED_ACK_MS &&
                     tcp_counters.delayed_acks == delayed + 1,
                 "small in-order segment: ACK delayed %u ms",
                 TCP_DELAYED_ACK_MS);
    expire_next(c);
    ktest_assert(frame_count == before + 1 && net_clock_ms() == start + TCP_DELAYED_ACK_MS &&
                     last().acknowledgement == c->rcv_nxt &&
                     tcp_counters.delayed_ack_timeouts == timeouts + 1,
                 "delayed ACK sent by its timer after 100 ms");

    before = frame_count;
    peer_data(c, c->rcv_nxt, 1460);
    ktest_assert(frame_count == before, "first full segment waits");
    peer_data(c, c->rcv_nxt, 1460);
    ktest_assert(frame_count == before + 1 && last().acknowledgement == c->rcv_nxt &&
                     !c->ack_deadline,
                 "second full segment acknowledged at once, one ACK for both");

    before = frame_count;
    peer_data(c, c->rcv_nxt, 50);
    ktest_assert(c->ack_deadline && send_bytes(file, "reply", 5) == 5 &&
                     frame_count == before + 1 && last().payload == 5 &&
                     last().acknowledgement == c->rcv_nxt && !c->ack_deadline,
                 "outgoing data carries the delayed ACK");

    /* Under receiver silly window avoidance a small read sends no update,
     * and a read of a full segment does. */
    struct socket_msg message = {.data = pattern, .len = 100};
    before = frame_count;
    ktest_assert(socket_recvmsg(socket_from_file(file), &message) == 100 && frame_count == before,
                 "100-byte read sends no window update");
    message.len = 3000;
    ktest_assert(socket_recvmsg(socket_from_file(file), &message) > 0 &&
                     frame_count == before + 1 && last().flags == TCP_ACK,
                 "read of more than a segment announces the window");
    reset(c);
    file_put(file);
}

/* Grows the congestion window with full ACKs until it holds at least
 * `segments` segments of 1000 bytes, then leaves a full flight
 * outstanding. */
static uint32_t fill_flight(struct file *file, struct tcp_connection *c, unsigned segments)
{
    static char data[TCP_SEND_CAPACITY];
    for (;;) {
        size_t room = TCP_SEND_CAPACITY - c->transmit_length;
        ktest_assert(!room || send_bytes(file, data, room) == (long)room, "refill send store");
        if (c->congestion_window >= segments * 1000u)
            break;
        peer_ack(c, c->snd_nxt, 0, NULL);
    }
    /* One flush sends at most eight segments. */
    while (c->transmit_sent < c->congestion_window)
        tcp_flush(c);
    ktest_assert(c->transmit_sent >= segments * 1000u,
                 "flight of %u bytes",
                 (unsigned)c->transmit_sent);
    return c->snd_una;
}

static void recovery_checks(void)
{
    struct file *file = sack_open(false);
    struct tcp_connection *c = connection_of(file);
    uint32_t u = fill_flight(file, c, 8);
    ktest_assert(c->transmit_sent == 8000, "flight of exactly eight segments");
    uint64_t retransmits = tcp_counters.retransmits, sack_rtx = tcp_counters.sack_retransmits;
    uint64_t recoveries = tcp_counters.sack_recoveries;

    /* Segment 0 is lost; the peer SACKs segments 1, 2 and 3. */
    uint32_t sack[4][2] = {{u + 1000, u + 2000}};
    peer_ack(c, u, 1, sack);
    sack[0][1] = u + 3000;
    peer_ack(c, u, 1, sack);
    ktest_assert(!c->recovering && c->duplicate_acks == 2 && c->scoreboard_count == 1,
                 "two duplicates do not start recovery");
    unsigned before = frame_count;
    sack[0][1] = u + 4000;
    peer_ack(c, u, 1, sack);
    struct decoded d = last();
    ktest_assert(c->recovering && tcp_counters.sack_recoveries == recoveries + 1 &&
                     frame_count == before + 1 && d.sequence == u && d.payload == 1000,
                 "third duplicate retransmits the first lost segment");
    ktest_assert(c->congestion_window == 4000 && c->slow_start_threshold == 4000 &&
                     c->recovery_end == u + 8000 && c->high_rxt == u + 1000,
                 "window and threshold halved, recovery point set");

    /* The pipe (1000 retransmitted + 4000 above the SACKed range) exceeds
     * the window; one more SACKed segment leaves it at the window. */
    before = frame_count;
    sack[0][1] = u + 5000;
    peer_ack(c, u, 1, sack);
    ktest_assert(frame_count == before, "pipe at the window: nothing sent");
    sack[0][1] = u + 6000;
    peer_ack(c, u, 1, sack);
    d = last();
    ktest_assert(frame_count == before + 1 && d.sequence == u + 8000,
                 "room in the pipe sends new data, not SACKed data");
    ktest_assert(tcp_counters.retransmits == retransmits + 1 &&
                     tcp_counters.sack_retransmits == sack_rtx + 1,
                 "exactly one retransmission during SACK recovery");

    /* A cumulative ACK beyond the recovery point ends recovery. */
    peer_ack(c, u + 8000, 0, NULL);
    ktest_assert(!c->recovering && c->congestion_window == 4000 && !c->scoreboard_count,
                 "recovery ends at the recovery point");
    reset(c);
    file_put(file);
}

static void timeout_checks(void)
{
    struct file *file = sack_open(false);
    struct tcp_connection *c = connection_of(file);
    uint32_t u = fill_flight(file, c, 8);
    ktest_assert(c->transmit_sent == 8000, "flight of exactly eight segments");
    uint32_t sack[1][2] = {{u + 2000, u + 4000}};
    peer_ack(c, u, 1, sack);
    ktest_assert(!c->recovering && c->scoreboard_count == 1, "one duplicate with a SACK block");
    expire_next(c);
    ktest_assert(c->rto_recovery && last().sequence == u && c->congestion_window == 1000,
                 "timeout retransmits the first segment and keeps the scoreboard");
    unsigned before = frame_count;
    peer_ack(c, u + 1000, 1, sack);
    ktest_assert(frame_count == before + 2 && decode_frame(before).sequence == u + 1000 &&
                     decode_frame(before + 1).sequence == u + 4000,
                 "after the timeout the SACKed range is skipped");
    expire_next(c);
    ktest_assert(c->scoreboard_count == 1, "one timeout after progress keeps the scoreboard");
    expire_next(c);
    ktest_assert(!c->scoreboard_count, "second consecutive timeout clears the scoreboard");
    reset(c);
    file_put(file);
}

static void scoreboard_checks(void)
{
    struct file *file = sack_open(false);
    struct tcp_connection *c = connection_of(file);
    uint32_t u = fill_flight(file, c, 20);
    uint64_t drops = tcp_counters.scoreboard_drops, received = tcp_counters.sack_blocks_received;
    /* The blocks lie below snd_una, lie beyond snd_nxt and are empty, in
     * this order. */
    uint32_t sack[3][2] = {
        {u - 500, u},
        {u + 30000, u + 31000},
        {u + 1000, u + 1000},
    };
    peer_ack(c, u, 3, sack);
    ktest_assert(!c->scoreboard_count && tcp_counters.sack_blocks_received == received,
                 "invalid blocks ignored");
    for (unsigned i = 0; i < 9; i++) {
        uint32_t block[1][2] = {{u + 2000 * i + 1000, u + 2000 * i + 1500}};
        peer_ack(c, u, 1, block);
    }
    ktest_assert(c->scoreboard_count == TCP_SCOREBOARD &&
                     tcp_counters.scoreboard_drops == drops + 1 &&
                     c->scoreboard[0].start == u + 1000 &&
                     c->scoreboard[TCP_SCOREBOARD - 1].start == u + 2000 * 7 + 1000,
                 "scoreboard bounded to %u ranges, the highest dropped",
                 TCP_SCOREBOARD);
    uint32_t cover[1][2] = {{u + 1000, u + 16000}};
    peer_ack(c, u, 1, cover);
    ktest_assert(c->scoreboard_count == 1 && c->scoreboard[0].end == u + 16000,
                 "a covering block merges every range");
    reset(c);
    file_put(file);
}

/* A FIN at RCV.NXT is accepted when the store is full and the window is
 * closed, since it needs no space. */
static void closed_window_checks(void)
{
    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    struct peer_options options = {.mss = 1000, .window_scale = -1};
    inject(c, 100, TCP_SYN | TCP_ACK, 4096, &options, NULL, 0);
    struct peer_options none = {.window_scale = -1};
    static char data[8000];
    for (unsigned window; (window = tcp_receive_window(c));)
        inject(c, c->rcv_nxt, TCP_ACK, 4096, &none, data, MIN(window, sizeof data));
    uint32_t end = c->rcv_nxt;
    inject(c, end, TCP_ACK | TCP_FIN, 4096, &none, NULL, 0);
    ktest_assert(c->state == TCP_CLOSE_WAIT && c->rcv_nxt == end + 1 &&
                     last().acknowledgement == end + 1,
                 "FIN accepted in a closed window");
    reset(c);
    file_put(file);
}

static void unsacked_checks(void)
{
    /* When the peer does not permit SACK, no block is ever sent. */
    struct file *file = active_open();
    struct tcp_connection *c = connection_of(file);
    struct peer_options options = {.mss = 1000, .window_scale = -1};
    inject(c, 100, TCP_SYN | TCP_ACK, 4096, &options, NULL, 0);
    ktest_assert(!c->sack, "SACK not negotiated");
    struct peer_options none = {.window_scale = -1};
    inject(c, c->rcv_nxt + 100, TCP_ACK, 4096, &none, "gap", 3);
    ktest_assert(!last().sack_count && last().acknowledgement == c->rcv_nxt,
                 "out-of-order data without SACK: plain duplicate ACK");
    reset(c);
    file_put(file);
}

static int controlled_sack(struct net_request *request)
{
    ktest_assert(netif_register(&fake) == 0, "register SACK capture interface");
    netif_set_up(&fake, true);
    ktest_assert(net_configure(&fake, LOCAL, 0xffffff00u, 0) == 0, "SACK test address");
    for (unsigned i = 0; i < sizeof pattern; i++)
        pattern[i] = (char)(i * 13);
    net_clock_control(true);
    tcp_set_generators(initial_sequence, next_port);
    receiver_checks(false);
    receiver_checks(true);
    delayed_ack_checks();
    recovery_checks();
    timeout_checks();
    scoreboard_checks();
    unsacked_checks();
    closed_window_checks();
    tcp_set_generators(NULL, NULL);
    struct tcp_stats stats;
    ktest_assert(tcp_get_stats(&stats) == 0 && !stats.connections && !stats.endpoints,
                 "all SACK test connections released");
    net_clock_control(false);
    netif_set_up(&fake, false);
    return 0;
}

static void test_tcp_sack(void)
{
    struct pbuf_stats before, after;
    pbuf_get_stats(&before);
    struct net_request request;
    net_request_init(&request, controlled_sack);
    ktest_assert(net_request_run(&request) == 0, "controlled SACK checks");
    net_worker_drain();
    pbuf_get_stats(&after);
    ktest_assert(before.free == after.free, "packet pool restored %u/%u", before.free, after.free);
    kprintf("net_tcp_sack: SACK blocks, scoreboard, RFC 6675 recovery and delayed ACKs, ok\n");
}
KTEST_DEFINE("net_tcp_sack", test_tcp_sack);
