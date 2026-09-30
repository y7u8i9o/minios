/* Mode scripted of netpeer: a scripted TCP peer on the isolated dgram link.
 *
 * QEMU user networking terminates guest TCP in its own stack, which offers
 * only the MSS option, so it cannot show the options of N13 and later
 * milestones being used. This peer owns 10.0.0.1 on the raw link, answers
 * ARP, accepts one connection on port 7000 and drives it through a fixed
 * script. It is written independently of the guest's parsers: it builds
 * and checks every header itself and reports what it observed, one line
 * per step, in its log. The post script of the case reads those lines and
 * the capture checker validates every frame again.
 *
 * The script: offer MSS 1400, window scale 5 and timestamps in the SYN
 * ACK; receive SCRIPT_GUEST_BYTES from the guest while advertising a
 * scaled window, acknowledging after four segments or 20 ms of silence;
 * acknowledge the guest's FIN; send one segment with a timestamp older
 * than the last one, which PAWS must reject; then send SCRIPT_PEER_BYTES
 * with fresh timestamps and a FIN, and wait until the guest has
 * acknowledged everything. */
#include "scripted.h"
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#define PEER_IP 0x0a000001u
#define GUEST_IP 0x0a000002u
#define SCRIPT_PORT 7000
#define PEER_MSS 1400
#define PEER_SHIFT 5
#define PEER_WINDOW 1024 /* field value: 32768 bytes after the shift */
#define SCRIPT_GUEST_BYTES 100000
#define SCRIPT_PEER_BYTES 3000

static const unsigned char peer_mac[6] = {2, 0, 0, 0, 0, 1};

struct segment {
    uint8_t flags;
    uint16_t source_port, window;
    uint32_t sequence, acknowledgement;
    const unsigned char *data;
    size_t length;
    int has_mss, has_scale, has_timestamp, sack_permitted;
    unsigned mss, shift;
    uint32_t ts_value, ts_echo;
};

struct script {
    int fd;
    const struct sockaddr_in *guest;
    FILE *log;
    double start;
    unsigned char guest_mac[6];
    uint16_t guest_port;
    uint32_t iss, snd_una, snd_nxt; /* our sequence space */
    uint32_t rcv_nxt;               /* next guest byte */
    int guest_shift;                /* -1: the guest did not offer scaling */
    int timestamps;
    uint32_t ts_recent;             /* newest in-order guest timestamp */
    uint32_t ts_first, ts_last;     /* the range of timestamps we sent */
    unsigned long received, pattern_errors, missing_ts, echo_errors;
    unsigned long beyond_unscaled, max_flight, guest_window;
    uint32_t last_ack_sent;
};

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put16(unsigned char *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 255;
}
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v & 255;
}
static uint32_t fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    return sum;
}
static uint32_t sum_bytes(const unsigned char *p, size_t n, uint32_t sum)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += get16(p + i);
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    return fold(sum);
}
static int before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static double now_ms(const struct script *s)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((double)tv.tv_sec + tv.tv_usec / 1e6 - s->start) * 1000.0;
}
static uint32_t timestamp(struct script *s)
{
    uint32_t value = 100000u + (uint32_t)now_ms(s);
    if (!s->ts_first)
        s->ts_first = value;
    s->ts_last = value;
    return value;
}

static unsigned char guest_byte(unsigned long i)
{
    return (unsigned char)(i * 31 + (i >> 9));
}
static unsigned char peer_byte(unsigned long i)
{
    return (unsigned char)(i * 17 + 3);
}

/* Sends one segment. ts_value 0 selects a fresh timestamp. */
static void send_segment(struct script *s, uint8_t flags, uint32_t sequence,
                         const unsigned char *data, size_t length, uint32_t ts_value)
{
    unsigned char frame[1600];
    unsigned char *ip = frame + 14, *tcp = ip + 20, *o = tcp + 20;
    size_t options = 0;
    if (flags & 0x02) {
        o[0] = 2;
        o[1] = 4;
        put16(o + 2, PEER_MSS);
        options = 4;
        if (s->guest_shift >= 0) {
            o[4] = 1;
            o[5] = 3;
            o[6] = 3;
            o[7] = PEER_SHIFT;
            options = 8;
        }
    }
    if (s->timestamps) {
        o[options] = 1;
        o[options + 1] = 1;
        o[options + 2] = 8;
        o[options + 3] = 10;
        put32(o + options + 4, ts_value ? ts_value : timestamp(s));
        put32(o + options + 8, s->ts_recent);
        options += 12;
    }
    size_t tcp_length = 20 + options + length;
    memcpy(frame, s->guest_mac, 6);
    memcpy(frame + 6, peer_mac, 6);
    put16(frame + 12, 0x0800);
    memset(ip, 0, 20);
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(20 + tcp_length));
    put16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 6;
    put32(ip + 12, PEER_IP);
    put32(ip + 16, GUEST_IP);
    put16(ip + 10, (uint16_t)~sum_bytes(ip, 20, 0));
    memset(tcp, 0, 20);
    put16(tcp, SCRIPT_PORT);
    put16(tcp + 2, s->guest_port);
    put32(tcp + 4, sequence);
    put32(tcp + 8, flags & 0x10 ? s->rcv_nxt : 0);
    tcp[12] = (unsigned char)((20 + options) / 4) << 4;
    tcp[13] = flags;
    put16(tcp + 14, flags & 0x02 ? 2048 : PEER_WINDOW);
    if (length)
        memcpy(tcp + 20 + options, data, length);
    unsigned char pseudo[12];
    put32(pseudo, PEER_IP);
    put32(pseudo + 4, GUEST_IP);
    pseudo[8] = 0;
    pseudo[9] = 6;
    put16(pseudo + 10, (uint16_t)tcp_length);
    put16(tcp + 16, (uint16_t)~sum_bytes(tcp, tcp_length, sum_bytes(pseudo, 12, 0)));
    if (flags & 0x10)
        s->last_ack_sent = s->rcv_nxt;
    sendto(s->fd, frame, 34 + tcp_length, 0, (const struct sockaddr *)s->guest, sizeof *s->guest);
}

static void send_ack(struct script *s)
{
    send_segment(s, 0x10, s->snd_nxt, NULL, 0, 0);
}

/* Answers an ARP request for our address; returns 1 when it was one. */
static int answer_arp(struct script *s, unsigned char *b, size_t n)
{
    if (n < 42 || get16(b + 12) != 0x0806 || get16(b + 20) != 1 || get32(b + 38) != PEER_IP)
        return 0;
    unsigned char reply[42];
    memcpy(reply, b + 6, 6);
    memcpy(reply + 6, peer_mac, 6);
    put16(reply + 12, 0x0806);
    memcpy(reply + 14, b + 14, 6);
    put16(reply + 20, 2);
    memcpy(reply + 22, peer_mac, 6);
    put32(reply + 28, PEER_IP);
    memcpy(reply + 32, b + 22, 10);
    sendto(s->fd, reply, sizeof reply, 0, (const struct sockaddr *)s->guest, sizeof *s->guest);
    return 1;
}

/* Parses a guest TCP segment to our port; returns 0 for anything else. */
static int parse(struct script *s, const unsigned char *b, size_t n, struct segment *out)
{
    if (n < 54 || get16(b + 12) != 0x0800 || b[14] != 0x45 || b[23] != 6 ||
        sum_bytes(b + 14, 20, 0) != 0xffff)
        return 0;
    size_t total = get16(b + 16);
    if (total < 40 || total + 14 > n || get32(b + 26) != GUEST_IP || get32(b + 30) != PEER_IP)
        return 0;
    const unsigned char *tcp = b + 34;
    size_t tcp_length = total - 20, header = (size_t)(tcp[12] >> 4) * 4;
    unsigned char pseudo[12];
    put32(pseudo, GUEST_IP);
    put32(pseudo + 4, PEER_IP);
    pseudo[8] = 0;
    pseudo[9] = 6;
    put16(pseudo + 10, (uint16_t)tcp_length);
    if (header < 20 || header > tcp_length || get16(tcp + 2) != SCRIPT_PORT ||
        sum_bytes(tcp, tcp_length, sum_bytes(pseudo, 12, 0)) != 0xffff)
        return 0;
    memset(out, 0, sizeof *out);
    memcpy(s->guest_mac, b + 6, 6);
    out->source_port = get16(tcp);
    out->sequence = get32(tcp + 4);
    out->acknowledgement = get32(tcp + 8);
    out->flags = tcp[13];
    out->window = get16(tcp + 14);
    out->data = tcp + header;
    out->length = tcp_length - header;
    for (size_t o = 20; o < header;) {
        if (tcp[o] == 0)
            break;
        if (tcp[o] == 1) {
            o++;
            continue;
        }
        if (o + 2 > header || tcp[o + 1] < 2 || o + tcp[o + 1] > header)
            return 0;
        if (tcp[o] == 2 && tcp[o + 1] == 4) {
            out->has_mss = 1;
            out->mss = get16(tcp + o + 2);
        } else if (tcp[o] == 3 && tcp[o + 1] == 3) {
            out->has_scale = 1;
            out->shift = tcp[o + 2];
        } else if (tcp[o] == 4 && tcp[o + 1] == 2) {
            out->sack_permitted = 1;
        } else if (tcp[o] == 8 && tcp[o + 1] == 10) {
            out->has_timestamp = 1;
            out->ts_value = get32(tcp + o + 2);
            out->ts_echo = get32(tcp + o + 6);
        }
        o += tcp[o + 1];
    }
    return 1;
}

/* Waits up to timeout_ms for the next guest segment, answering ARP on the
 * way. Returns 1 with a segment, 0 on timeout, -1 when stopping. */
static int next_segment(struct script *s, int timeout_ms, volatile sig_atomic_t *stopping,
                        unsigned char *buffer, size_t size, struct segment *out)
{
    double deadline = now_ms(s) + timeout_ms;
    while (!*stopping) {
        int left = (int)(deadline - now_ms(s));
        if (left <= 0)
            return 0;
        struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
        int ready = poll(&pfd, 1, left);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return 0;
        ssize_t n = recv(s->fd, buffer, size, 0);
        if (n <= 0)
            continue;
        if (answer_arp(s, buffer, (size_t)n))
            continue;
        if (parse(s, buffer, (size_t)n, out))
            return 1;
    }
    return -1;
}

/* Timestamp checks that apply to every guest segment after the SYN. */
static void check_timestamp(struct script *s, const struct segment *g)
{
    if (!s->timestamps)
        return;
    if (!g->has_timestamp) {
        s->missing_ts++;
        return;
    }
    if (before(g->ts_echo, s->ts_first) || before(s->ts_last, g->ts_echo))
        s->echo_errors++;
    if (!before(g->ts_value, s->ts_recent) && !before(s->last_ack_sent, g->sequence))
        s->ts_recent = g->ts_value;
}

void scripted_peer(int fd, const struct sockaddr_in *guest, FILE *log, volatile sig_atomic_t *stopping)
{
    struct script s;
    memset(&s, 0, sizeof s);
    s.fd = fd;
    s.guest = guest;
    s.log = log;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    s.start = (double)tv.tv_sec + tv.tv_usec / 1e6;
    s.iss = 1000000;
    s.guest_shift = -1;
    unsigned char buffer[65536];
    struct segment g;

    /* Handshake. */
    for (;;) {
        int r = next_segment(&s, 1000, stopping, buffer, sizeof buffer, &g);
        if (r < 0)
            return;
        if (r > 0 && (g.flags & 0x12) == 0x02)
            break;
    }
    fprintf(log, "script syn mss %u wscale %d sackok %d ts %d\n", g.mss,
            g.has_scale ? (int)g.shift : -1, g.sack_permitted, g.has_timestamp);
    s.guest_port = g.source_port;
    s.guest_shift = g.has_scale ? (int)g.shift : -1;
    s.timestamps = g.has_timestamp;
    s.ts_recent = g.ts_value;
    s.rcv_nxt = g.sequence + 1;
    s.snd_una = s.iss;
    s.snd_nxt = s.iss + 1;
    send_segment(&s, 0x12, s.iss, NULL, 0, 0);
    for (;;) {
        int r = next_segment(&s, 3000, stopping, buffer, sizeof buffer, &g);
        if (r <= 0) {
            fprintf(log, "script handshake failed\n");
            return;
        }
        if ((g.flags & 0x10) && g.acknowledgement == s.snd_nxt && !(g.flags & 0x02))
            break;
        if (g.flags & 0x02)
            send_segment(&s, 0x12, s.iss, NULL, 0, 0);
    }
    s.snd_una = s.snd_nxt;
    unsigned shift = s.guest_shift >= 0 ? (unsigned)s.guest_shift : 0;
    check_timestamp(&s, &g);
    s.guest_window = (unsigned long)g.window << shift;

    /* Receive the guest's stream. In-order bytes are checked against the
     * pattern; the ACK is held back until four segments or 20 ms of
     * silence, so the guest's flight shows how far it trusts our window. */
    unsigned pending = 0;
    int fin = 0;
    while (!fin) {
        int r = next_segment(&s, 20, stopping, buffer, sizeof buffer, &g);
        if (r < 0)
            return;
        if (r == 0) {
            if (pending)
                send_ack(&s);
            pending = 0;
            continue;
        }
        check_timestamp(&s, &g);
        if ((unsigned long)g.window << shift > s.guest_window)
            s.guest_window = (unsigned long)g.window << shift;
        uint32_t end = g.sequence + (uint32_t)g.length;
        uint32_t unscaled_edge = s.last_ack_sent + PEER_WINDOW;
        if (g.length && before(unscaled_edge, end))
            s.beyond_unscaled++;
        if (g.length && (unsigned long)(end - s.last_ack_sent) > s.max_flight)
            s.max_flight = end - s.last_ack_sent;
        if (g.sequence == s.rcv_nxt && g.length) {
            for (size_t i = 0; i < g.length; i++)
                if (g.data[i] != guest_byte(s.received + i))
                    s.pattern_errors++;
            s.received += g.length;
            s.rcv_nxt += (uint32_t)g.length;
            pending++;
        } else if (g.length) {
            pending = 4; /* duplicate or out of order: acknowledge at once */
        }
        if ((g.flags & 0x01) && g.sequence + g.length == s.rcv_nxt) {
            s.rcv_nxt++;
            fin = 1;
            pending = 4;
        }
        if (pending >= 4) {
            send_ack(&s);
            pending = 0;
        }
    }
    fprintf(log, "script received %lu bytes pattern errors %lu\n", s.received, s.pattern_errors);
    fprintf(log, "script max flight %lu beyond unscaled window %lu\n", s.max_flight,
            s.beyond_unscaled);
    fprintf(log, "script guest window %lu\n", s.guest_window);

    /* PAWS: the first data segment goes out with a timestamp older than
     * any the guest has seen from us. It must not be acknowledged. */
    unsigned char data[SCRIPT_PEER_BYTES];
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = peer_byte(i);
    if (s.timestamps) {
        send_segment(&s, 0x18, s.snd_nxt, data, 1000, s.ts_first - 50000);
        int answered = 0;
        for (;;) {
            int r = next_segment(&s, 500, stopping, buffer, sizeof buffer, &g);
            if (r <= 0)
                break;
            check_timestamp(&s, &g);
            if ((g.flags & 0x10) && !g.length) {
                answered = g.acknowledgement == s.snd_nxt ? 1 : -1;
                break;
            }
        }
        fprintf(log, "script paws %s\n", answered > 0   ? "old segment answered without acceptance"
                                         : answered < 0 ? "old segment accepted"
                                                        : "no answer");
    }

    /* The stream with fresh timestamps, then FIN. */
    for (size_t offset = 0; offset < sizeof data; offset += 1000)
        send_segment(&s, 0x18 | (offset + 1000 == sizeof data ? 0x01 : 0), s.snd_nxt + offset,
                     data + offset, 1000, 0);
    uint32_t goal = s.snd_nxt + sizeof data + 1;
    int acknowledged = 0;
    for (int round = 0; round < 50 && !acknowledged; round++) {
        int r = next_segment(&s, 100, stopping, buffer, sizeof buffer, &g);
        if (r < 0)
            return;
        if (r == 0)
            continue;
        check_timestamp(&s, &g);
        if ((g.flags & 0x10) && g.acknowledgement == goal)
            acknowledged = 1;
    }
    s.snd_nxt = goal;
    fprintf(log, "script sent %u bytes and FIN %s\n", (unsigned)sizeof data,
            acknowledged ? "acknowledged" : "not acknowledged");
    fprintf(log, "script timestamps missing %lu echo errors %lu\n", s.missing_ts, s.echo_errors);
    fprintf(log, "script complete\n");
    while (!*stopping) {
        int r = next_segment(&s, 1000, stopping, buffer, sizeof buffer, &g);
        if (r > 0 && g.length)
            send_ack(&s);
    }
}
