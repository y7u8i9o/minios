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
 * The script offers MSS 1400, window scale 5, SACK and timestamps in the
 * SYN ACK. It receives SCRIPT_GUEST_BYTES from the guest while advertising
 * a scaled window and acknowledging in-order data after four segments or
 * 20 ms of silence. The first transmission of the segment that reaches
 * byte LOSS_OFFSET is dropped; every later segment is stored out of order
 * and answered at once with SACK blocks, so the guest has to repair the
 * hole from its scoreboard. After the guest's FIN the peer sends one
 * segment with a timestamp older than any before, which PAWS must reject,
 * sends three 1000-byte segments in the order 3, 1, 2 and checks the
 * guest's SACK blocks, sends 100 bytes alone and times the delayed ACK,
 * sends two full segments back to back and counts the ACKs, and finally
 * sends a FIN and waits until the guest has acknowledged everything. */
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
/* The window field is 1024, which is 32768 bytes after the shift. A full
 * guest segment is the guest's MSS 1460 minus the timestamp option. */
#define PEER_WINDOW 1024
#define SCRIPT_GUEST_BYTES 100000
#define LOSS_OFFSET 50000
#define GUEST_FULL 1448
#define SCRIPT_PEER_BYTES (3000 + 100 + 2 * GUEST_FULL)
#define MAX_RANGES 32

static const unsigned char peer_mac[6] = {2, 0, 0, 0, 0, 1};

struct segment {
    uint8_t flags;
    uint16_t source_port, window;
    uint32_t sequence, acknowledgement;
    const unsigned char *data;
    size_t length;
    int has_mss, has_scale, has_timestamp, sack_permitted;
    unsigned mss, shift, sack_count;
    uint32_t ts_value, ts_echo;
    uint32_t sack[4][2];
};

struct range {
    uint32_t start, end;
};

/* iss and snd_nxt describe our sequence space, guest_iss and rcv_nxt the
 * guest's. guest_shift is -1 when the guest did not offer scaling.
 * ts_recent is the newest in-order guest timestamp, and ts_first and
 * ts_last bound the timestamps we sent. held lists the out-of-order guest
 * data we hold and recent the range reported first. The fields from
 * dropped onwards record the deliberate loss. */
struct script {
    int fd;
    const struct sockaddr_in *guest;
    FILE *log;
    double start;
    unsigned char guest_mac[6];
    uint16_t guest_port;
    uint32_t iss, snd_nxt;
    uint32_t guest_iss, rcv_nxt;
    int guest_shift;
    int timestamps, sack;
    uint32_t ts_recent;
    uint32_t ts_first, ts_last;
    uint32_t last_ack_sent;
    unsigned long received, pattern_errors, missing_ts, echo_errors;
    unsigned long beyond_unscaled, max_flight, guest_window;
    struct range held[MAX_RANGES];
    unsigned held_count;
    struct range recent;
    int dropped, repaired;
    uint32_t drop_sequence, drop_ts;
    unsigned long sacked_resent, repair_ms;
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

/* send_segment sends one segment. A ts_value of 0 selects a fresh
 * timestamp. An ACK reports
 * the held ranges as SACK blocks, the most recent one first. */
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
            memcpy(o + options, "\x01\x03\x03", 3);
            o[options + 3] = PEER_SHIFT;
            options += 4;
        }
        if (s->sack) {
            memcpy(o + options, "\x01\x01\x04\x02", 4);
            options += 4;
        }
    }
    if (s->timestamps) {
        memcpy(o + options, "\x01\x01\x08\x0a", 4);
        put32(o + options + 4, ts_value ? ts_value : timestamp(s));
        put32(o + options + 8, s->ts_recent);
        options += 12;
    }
    if (s->sack && s->held_count && (flags & 0x12) == 0x10) {
        unsigned blocks = 0;
        unsigned char *b = o + options + 4;
        put32(b, s->recent.start);
        put32(b + 4, s->recent.end);
        blocks = 1;
        for (unsigned i = 0; i < s->held_count && blocks < 3; i++) {
            if (s->held[i].start == s->recent.start)
                continue;
            put32(b + 8 * blocks, s->held[i].start);
            put32(b + 8 * blocks + 4, s->held[i].end);
            blocks++;
        }
        memcpy(o + options, "\x01\x01\x05", 3);
        o[options + 3] = (unsigned char)(2 + 8 * blocks);
        options += 4 + 8 * blocks;
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

/* answer_arp answers an ARP request for our address and returns 1 when
 * the frame was one. */
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

/* parse decodes a guest TCP segment to our port and returns 0 for anything
 * else. */
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
        unsigned length = tcp[o + 1];
        if (tcp[o] == 2 && length == 4) {
            out->has_mss = 1;
            out->mss = get16(tcp + o + 2);
        } else if (tcp[o] == 3 && length == 3) {
            out->has_scale = 1;
            out->shift = tcp[o + 2];
        } else if (tcp[o] == 4 && length == 2) {
            out->sack_permitted = 1;
        } else if (tcp[o] == 5 && length >= 10 && (length - 2) % 8 == 0 && length <= 34) {
            out->sack_count = (length - 2) / 8;
            for (unsigned i = 0; i < out->sack_count; i++) {
                out->sack[i][0] = get32(tcp + o + 2 + 8 * i);
                out->sack[i][1] = get32(tcp + o + 6 + 8 * i);
            }
        } else if (tcp[o] == 8 && length == 10) {
            out->has_timestamp = 1;
            out->ts_value = get32(tcp + o + 2);
            out->ts_echo = get32(tcp + o + 6);
        }
        o += length;
    }
    return 1;
}

/* next_segment waits up to timeout_ms for the next guest segment and
 * answers ARP on the way. It returns 1 with a segment, 0 on timeout and -1
 * when stopping. */
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

/* check_timestamp applies the timestamp checks to every guest segment
 * after the SYN. */
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

/* hold records [start, end) as held out of order, merges touching ranges
 * and makes the merged range the one reported first. */
static void hold(struct script *s, uint32_t start, uint32_t end)
{
    struct range merged = {start, end};
    unsigned kept = 0;
    for (unsigned i = 0; i < s->held_count; i++) {
        struct range r = s->held[i];
        if (before(r.end, merged.start) || before(merged.end, r.start)) {
            s->held[kept++] = r;
            continue;
        }
        if (before(r.start, merged.start))
            merged.start = r.start;
        if (before(merged.end, r.end))
            merged.end = r.end;
    }
    if (kept < MAX_RANGES)
        s->held[kept++] = merged;
    s->held_count = kept;
    s->recent = merged;
}

/* absorb moves rcv_nxt across held ranges that now continue the stream. */
static void absorb(struct script *s)
{
    for (int moved = 1; moved;) {
        moved = 0;
        for (unsigned i = 0; i < s->held_count; i++) {
            if (before(s->rcv_nxt, s->held[i].start))
                continue;
            if (before(s->rcv_nxt, s->held[i].end))
                s->rcv_nxt = s->held[i].end;
            s->held[i] = s->held[--s->held_count];
            moved = 1;
            break;
        }
    }
}

static int is_held(const struct script *s, uint32_t start, uint32_t end)
{
    for (unsigned i = 0; i < s->held_count; i++)
        if (!before(start, s->held[i].start) && !before(s->held[i].end, end))
            return 1;
    return 0;
}

/* The byte at offset i of the guest's stream is guest_byte(i). */
static void check_pattern(struct script *s, const struct segment *g)
{
    for (size_t i = 0; i < g->length; i++)
        if (g->data[i] != guest_byte((uint32_t)(g->sequence - s->guest_iss - 1) + i))
            s->pattern_errors++;
}

/* receive_stream receives the guest's stream up to its FIN and returns 0
 * when stopping. */
static int receive_stream(struct script *s, volatile sig_atomic_t *stopping,
                          unsigned char *buffer, size_t size)
{
    struct segment g;
    unsigned shift = s->guest_shift >= 0 ? (unsigned)s->guest_shift : 0;
    unsigned pending = 0;
    for (;;) {
        int r = next_segment(s, 20, stopping, buffer, size, &g);
        if (r < 0)
            return 0;
        if (r == 0) {
            if (pending)
                send_ack(s);
            pending = 0;
            continue;
        }
        check_timestamp(s, &g);
        if ((unsigned long)g.window << shift > s->guest_window)
            s->guest_window = (unsigned long)g.window << shift;
        uint32_t end = g.sequence + (uint32_t)g.length;
        uint32_t offset = g.sequence - s->guest_iss - 1;
        if (g.length && before(s->last_ack_sent + PEER_WINDOW, end))
            s->beyond_unscaled++;
        if (g.length && (unsigned long)(end - s->last_ack_sent) > s->max_flight)
            s->max_flight = end - s->last_ack_sent;
        if (g.length && !s->dropped && offset + g.length > LOSS_OFFSET) {
            s->dropped = 1;
            s->drop_sequence = g.sequence;
            s->drop_ts = g.ts_value;
            fprintf(s->log, "script dropped guest segment at offset %u length %zu\n",
                    (unsigned)offset, g.length);
            continue;
        }
        if (g.length && s->dropped && !s->repaired && g.sequence == s->drop_sequence) {
            s->repaired = 1;
            s->repair_ms = g.ts_value - s->drop_ts;
        }
        if (g.length && is_held(s, g.sequence, end))
            s->sacked_resent++;
        int in_order = g.length && g.sequence == s->rcv_nxt;
        if (g.length && before(s->rcv_nxt, g.sequence)) {
            /* An out-of-order segment is held and answered by a duplicate
             * ACK with blocks. */
            check_pattern(s, &g);
            hold(s, g.sequence, end);
            send_ack(s);
            pending = 0;
        } else if (in_order) {
            check_pattern(s, &g);
            s->rcv_nxt = end;
            int had_hole = s->held_count > 0;
            absorb(s);
            if (had_hole) {
                send_ack(s);
                pending = 0;
            } else {
                pending++;
            }
        } else if (g.length) {
            /* A duplicate is acknowledged at once. */
            pending = 4;
        }
        if ((g.flags & 0x01) && end == s->rcv_nxt) {
            s->rcv_nxt++;
            send_ack(s);
            return 1;
        }
        if (pending >= 4) {
            send_ack(s);
            pending = 0;
        }
    }
}

/* await_ack waits for the next guest ACK without data and returns 1 with
 * it, or 0 after timeout_ms. elapsed receives the wait in milliseconds. */
static int await_ack(struct script *s, int timeout_ms, volatile sig_atomic_t *stopping,
                     unsigned char *buffer, size_t size, struct segment *g, double *elapsed)
{
    double started = now_ms(s);
    for (;;) {
        int left = timeout_ms - (int)(now_ms(s) - started);
        if (left <= 0)
            return 0;
        int r = next_segment(s, left, stopping, buffer, size, g);
        if (r <= 0)
            return 0;
        check_timestamp(s, g);
        if ((g->flags & 0x10) && !g->length) {
            if (elapsed)
                *elapsed = now_ms(s) - started;
            return 1;
        }
    }
}

void scripted_peer(int fd, const struct sockaddr_in *guest, FILE *log,
                   volatile sig_atomic_t *stopping)
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

    /* The handshake comes first. */
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
    s.sack = g.sack_permitted;
    s.ts_recent = g.ts_value;
    s.guest_iss = g.sequence;
    s.rcv_nxt = g.sequence + 1;
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
    check_timestamp(&s, &g);
    s.guest_window = (unsigned long)g.window << (s.guest_shift >= 0 ? s.guest_shift : 0);

    if (!receive_stream(&s, stopping, buffer, sizeof buffer))
        return;
    s.received = s.rcv_nxt - s.guest_iss - 2;
    fprintf(log, "script received %lu bytes pattern errors %lu\n", s.received, s.pattern_errors);
    fprintf(log, "script max flight %lu beyond unscaled window %lu\n", s.max_flight,
            s.beyond_unscaled);
    fprintf(log, "script guest window %lu\n", s.guest_window);
    fprintf(log, "script sack repair %s after %lu ms of guest time, sacked data resent %lu\n",
            s.repaired ? "retransmitted" : "missing", s.repair_ms, s.sacked_resent);

    unsigned char data[SCRIPT_PEER_BYTES];
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = peer_byte(i);
    uint32_t base = s.snd_nxt;
    double elapsed = 0;

    /* For PAWS the first data segment goes out with a timestamp older than
     * any the guest has seen from us. It must not be acknowledged. */
    if (s.timestamps) {
        send_segment(&s, 0x18, base, data, 1000, s.ts_first - 50000);
        int answered = await_ack(&s, 500, stopping, buffer, sizeof buffer, &g, NULL);
        fprintf(log, "script paws %s\n",
                !answered                     ? "no answer"
                : g.acknowledgement == base ? "old segment answered without acceptance"
                                              : "old segment accepted");
    }

    /* The segments go out of order, the third one first, then the first
     * and the second. The guest must report the held segment as a SACK
     * block. */
    int sack_ok = 1;
    send_segment(&s, 0x18, base + 2000, data + 2000, 1000, 0);
    if (!await_ack(&s, 500, stopping, buffer, sizeof buffer, &g, NULL) ||
        g.acknowledgement != base || !g.sack_count || g.sack[0][0] != base + 2000 ||
        g.sack[0][1] != base + 3000)
        sack_ok = 0;
    send_segment(&s, 0x18, base, data, 1000, 0);
    if (!await_ack(&s, 500, stopping, buffer, sizeof buffer, &g, NULL) ||
        g.acknowledgement != base + 1000 || !g.sack_count || g.sack[0][0] != base + 2000)
        sack_ok = 0;
    send_segment(&s, 0x18, base + 1000, data + 1000, 1000, 0);
    if (!await_ack(&s, 500, stopping, buffer, sizeof buffer, &g, NULL) ||
        g.acknowledgement != base + 3000 || g.sack_count)
        sack_ok = 0;
    fprintf(log, "script guest sack blocks %s\n", sack_ok ? "correct" : "wrong");

    /* The ACK of a lone small segment is delayed. */
    send_segment(&s, 0x18, base + 3000, data + 3000, 100, 0);
    int delayed = await_ack(&s, 1000, stopping, buffer, sizeof buffer, &g, &elapsed);
    fprintf(log, "script delayed ack %s after %.0f ms\n",
            delayed && g.acknowledgement == base + 3100 ? "received" : "missing", elapsed);

    /* One ACK covers two full segments sent back to back. */
    uint32_t full = base + 3100;
    send_segment(&s, 0x18, full, data + 3100, GUEST_FULL, 0);
    send_segment(&s, 0x18, full + GUEST_FULL, data + 3100 + GUEST_FULL, GUEST_FULL, 0);
    unsigned acks = 0;
    int covered = 0;
    while (await_ack(&s, 60, stopping, buffer, sizeof buffer, &g, NULL)) {
        acks++;
        covered |= g.acknowledgement == full + 2 * GUEST_FULL;
    }
    fprintf(log, "script two full segments acknowledged by %u ack%s%s\n", acks,
            acks == 1 ? "" : "s", covered ? " covering both" : "");

    /* The FIN goes out last, and the peer waits for its acknowledgement. */
    uint32_t goal = base + sizeof data + 1;
    send_segment(&s, 0x11, base + sizeof data, NULL, 0, 0);
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
