/* Network stack self tests (docs/plan/network.md): the controlled clock (N00),
 * the socket layer (N01) and the packet core with its worker (N02). They
 * run on the loopback interface and the kernel API alone; no device and
 * no user program is involved. */
#include <tests/ktest.h>
#include <net/clock.h>
#include <drivers/timer.h>
#include <console.h>

/* ---- N00: the controlled clock ---- */

static void test_net_clock(void)
{
    ktest_assert(!net_clock_is_controlled(), "the clock starts out uncontrolled");
    uint64_t real = timer_ms();
    uint64_t net = net_clock_ms();
    ktest_assert(net >= real && net - real < 50, "uncontrolled clock follows timer_ms: %lu vs %lu", net, real);

    net_clock_control(true);
    ktest_assert(net_clock_is_controlled(), "control taken");
    uint64_t frozen = net_clock_ms();
    sleep_ms(20);
    ktest_assert(net_clock_ms() == frozen, "controlled clock does not move on its own: %lu vs %lu",
                 net_clock_ms(), frozen);
    net_clock_advance(1000);
    ktest_assert(net_clock_ms() == frozen + 1000, "advance adds: %lu", net_clock_ms());
    net_clock_set(5);
    ktest_assert(net_clock_ms() == 5, "set replaces: %lu", net_clock_ms());
    net_clock_advance(7);
    ktest_assert(net_clock_ms() == 12, "advance after set: %lu", net_clock_ms());

    net_clock_control(false);
    ktest_assert(!net_clock_is_controlled(), "control released");
    net_clock_set(99);
    net_clock_advance(99);
    real = timer_ms();
    net = net_clock_ms();
    ktest_assert(net >= real && net - real < 50, "released clock follows timer_ms again: %lu vs %lu", net, real);
    kprintf("net_clock: ok\n");
}
KTEST_DEFINE("net_clock", test_net_clock);

/* ---- N01: the common socket layer ---- */

#include <ipc/socket.h>
#include <mm/slab.h>
#include <mm/pmm.h>
#include <lib/string.h>
#include <errno.h>

static void unix_name(struct sockaddr_storage *st, const char *name)
{
    struct sockaddr_un *sa = (struct sockaddr_un *)st;
    memset(st, 0, sizeof *st);
    sa->sun_family = AF_UNIX;
    strlcpy(sa->sun_path, name, sizeof sa->sun_path);
}

/* One connection cycle through the kernel API: listen, connect, accept,
 * a message each way with flags, names, options, then release. Every
 * error path is exercised as well. */
static void socket_cycle(int round)
{
    struct file *lf, *cf, *af;
    ktest_assert(socket_create(AF_UNIX, SOCK_STREAM, 0, 0, &lf) == 0, "listener");
    ktest_assert(socket_create(AF_UNIX, SOCK_STREAM, 0, O_NONBLOCK, &cf) == 0, "client");
    struct socket *l = socket_from_file(lf), *c = socket_from_file(cf);
    ktest_assert(l && c && socket_nonblocking(c) && !socket_nonblocking(l), "sockets behind the files");
    struct sockaddr_storage addr;
    unix_name(&addr, "net_socket");
    ktest_assert(socket_bind(l, &addr, sizeof(struct sockaddr_un)) == 0, "bind");
    ktest_assert(socket_bind(l, &addr, sizeof(struct sockaddr_un)) == -EINVAL, "second bind");
    addr.ss_family = AF_INET;
    ktest_assert(socket_bind(l, &addr, sizeof(struct sockaddr_un)) == -EAFNOSUPPORT, "bind with another family");
    addr.ss_family = AF_UNIX;
    ktest_assert(socket_listen(l, 4) == 0, "listen");
    ktest_assert(socket_connect(c, &addr, sizeof(struct sockaddr_un)) == 0, "connect");
    ktest_assert(socket_connect(c, &addr, sizeof(struct sockaddr_un)) == -EISCONN, "second connect");
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof peer;
    ktest_assert(socket_accept(l, 0, &af, &peer, &peerlen) == 0, "accept");
    ktest_assert(peerlen == sizeof(uint16_t) && peer.ss_family == AF_UNIX, "unnamed peer: %u", (unsigned)peerlen);
    struct socket *a = socket_from_file(af);
    peerlen = sizeof peer;
    ktest_assert(socket_getname(c, &peer, &peerlen, true) == 0 && peerlen == sizeof(struct sockaddr_un) &&
                 strcmp(((struct sockaddr_un *)&peer)->sun_path, "net_socket") == 0, "client's peer name");

    char data[32] = "hello";
    struct socket_msg m = { .data = data, .len = 5 };
    ktest_assert(socket_sendmsg(c, &m) == 5, "send");
    m.flags = MSG_OOB;
    ktest_assert(socket_sendmsg(c, &m) == -EOPNOTSUPP, "MSG_OOB rejected by the backend");
    m.flags = 0x10000;
    ktest_assert(socket_sendmsg(c, &m) == -EINVAL, "unknown flag rejected by the common layer");
    char in[32];
    struct socket_msg r = { .data = in, .len = sizeof in, .flags = MSG_PEEK };
    ktest_assert(socket_recvmsg(a, &r) == 5 && memcmp(in, "hello", 5) == 0, "peek");
    r.flags = 0;
    ktest_assert(socket_recvmsg(a, &r) == 5, "receive after the peek");
    r.flags = MSG_DONTWAIT;
    ktest_assert(socket_recvmsg(a, &r) == -EAGAIN, "empty with MSG_DONTWAIT");
    int v = -1;
    socklen_t vlen = sizeof v;
    ktest_assert(socket_getsockopt(a, SOL_SOCKET, SO_TYPE, &v, &vlen) == 0 && v == SOCK_STREAM, "SO_TYPE");
    socket_set_error(a, -ECONNRESET);
    vlen = sizeof v;
    ktest_assert(socket_getsockopt(a, SOL_SOCKET, SO_ERROR, &v, &vlen) == 0 && v == ECONNRESET, "SO_ERROR reports");
    vlen = sizeof v;
    ktest_assert(socket_getsockopt(a, SOL_SOCKET, SO_ERROR, &v, &vlen) == 0 && v == 0, "SO_ERROR is consumed");
    ktest_assert(socket_setsockopt(a, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof v) == -ENOPROTOOPT, "unsupported option");
    ktest_assert(socket_shutdown(a, 7) == -EINVAL, "bad shutdown");
    ktest_assert(socket_shutdown(a, SHUT_WR) == 0, "shutdown");
    r.flags = 0;
    ktest_assert(socket_recvmsg(c, &r) == 0, "end of file after the peer's shutdown");
    /* The order of release alternates so both sides see a live and a
     * dead peer. */
    if (round & 1) {
        file_put(cf);
        file_put(af);
    } else {
        file_put(af);
        file_put(cf);
    }
    file_put(lf);
    /* Families and types that are not there. */
    struct file *f;
    ktest_assert(socket_create(77, SOCK_STREAM, 0, 0, &f) == -EAFNOSUPPORT, "unknown family");
    ktest_assert(socket_create(AF_INET, SOCK_STREAM, 0, 0, &f) == 0, "AF_INET stream supported");
    file_put(f);
    ktest_assert(socket_create(AF_INET, SOCK_DGRAM, IPPROTO_TCP, 0, &f) == -EPROTONOSUPPORT, "datagram over TCP");
    ktest_assert(socket_create(AF_INET, SOCK_RAW, 0, 0, &f) == -ESOCKTNOSUPPORT, "raw");
    ktest_assert(socket_create(AF_UNIX, SOCK_DGRAM, 0, 0, &f) == -ESOCKTNOSUPPORT, "Unix datagram");
    ktest_assert(socket_pair(AF_INET, SOCK_STREAM, 0, 0, &f, &f) == -EOPNOTSUPP, "pair outside AF_UNIX");
}

/* The free page count once it has stopped moving: the files of released
 * sockets are freed by the rcu thread on whichever CPU runs it, so the
 * magazines and CPU page caches are drained before each reading and the
 * count must hold for ten readings five milliseconds apart. */
static uint64_t settled_free_pages(void)
{
    struct pmm_stats st;
    uint64_t last = 0;
    int stable = 0;
    for (int i = 0; i < 200 && stable < 10; i++) {
        sleep_ms(5);
        slab_reclaim();
        pmm_reclaim_cpu_caches();
        pmm_get_stats(&st);
        stable = st.free_pages == last ? stable + 1 : 0;
        last = st.free_pages;
    }
    return last;
}

static void test_net_socket(void)
{
    socket_cycle(0);                            /* warm the slab caches */
    /* Files are freed by an RCU callback; the baseline is taken once
     * the free page count has stopped moving. */
    uint64_t baseline = settled_free_pages();
    for (int i = 1; i <= 200; i++)
        socket_cycle(i);
    uint64_t after = settled_free_pages();
    ktest_assert(after == baseline, "200 cycles leaked %ld pages", (long)baseline - (long)after);
    kprintf("net_socket: ok\n");
}
KTEST_DEFINE("net_socket", test_net_socket);

/* ---- N02: packet buffers, checksums, the loopback interface, the
 * worker and its timers ---- */

#include <net/net.h>
#include <net/pbuf.h>
#include <net/netif.h>
#include <net/worker.h>
#include <net/loopback.h>
#include <net/checksum.h>
#include <net/byteorder.h>
#include <sched/sched.h>
#include <sched/thread.h>

static void test_pbuf(void)
{
    struct pbuf_stats st;
    pbuf_get_stats(&st);
    ktest_assert(st.free == st.total, "pool starts full: %u of %u", st.free, st.total);
    uint32_t total = st.total, reserve = st.reserve;
    uint64_t fails = st.alloc_fail;
    static struct pbuf *held[NET_PBUF_COUNT];
    uint32_t n = 0;
    /* Data allocations stop at the reserve, control allocations use it,
     * then everything fails and is counted. */
    while (n < NET_PBUF_COUNT && (held[n] = pbuf_alloc(PBUF_DATA)))
        n++;
    ktest_assert(n == total - reserve, "data allocations stop at the reserve: %u", n);
    while (n < NET_PBUF_COUNT && (held[n] = pbuf_alloc(PBUF_CONTROL)))
        n++;
    ktest_assert(n == total, "control allocations take the reserve: %u", n);
    ktest_assert(pbuf_alloc(PBUF_CONTROL) == NULL, "an empty pool fails");
    pbuf_get_stats(&st);
    ktest_assert(st.alloc_fail == fails + 2 && st.free == 0 && st.low_water == 0, "failures counted: %lu", st.alloc_fail);
    for (uint32_t i = 0; i < n; i++)
        pbuf_free(held[i]);
    pbuf_get_stats(&st);
    ktest_assert(st.free == total, "pool full again: %u", st.free);

    /* Ownership: every hand-over names its expectation. */
    struct pbuf *p = pbuf_alloc(PBUF_DATA);
    ktest_assert(p && p->owner == PBUF_OWNER_STACK && p->len == 0 && pbuf_headroom(p) == PBUF_HEADROOM, "fresh buffer");
    uint64_t bad = st.bad_transfer;
    ktest_assert(pbuf_transfer(p, PBUF_OWNER_STACK, PBUF_OWNER_DEVICE) == 0, "stack to device");
    ktest_assert(pbuf_transfer(p, PBUF_OWNER_STACK, PBUF_OWNER_QUEUE) == -EINVAL, "wrong expectation refused");
    ktest_assert(pbuf_transfer(p, PBUF_OWNER_DEVICE, PBUF_OWNER_POOL) == -EINVAL, "the pool is not a transfer target");
    ktest_assert(pbuf_transfer(p, PBUF_OWNER_DEVICE, PBUF_OWNER_STACK) == 0, "device to stack");
    pbuf_get_stats(&st);
    ktest_assert(st.bad_transfer == bad + 1, "one bad transfer counted: %lu", st.bad_transfer);

    /* Header room: push, pull, put and trim stay inside the buffer. */
    ktest_assert(pbuf_put(p, 100) != NULL && p->len == 100, "put");
    ktest_assert(pbuf_push(p, 14) == p->data && p->len == 114 && pbuf_headroom(p) == PBUF_HEADROOM - 14, "push");
    ktest_assert(pbuf_push(p, PBUF_HEADROOM) == NULL, "push beyond the headroom");
    ktest_assert(pbuf_pull(p, 14) != NULL && p->len == 100, "pull");
    ktest_assert(pbuf_pull(p, 101) == NULL, "pull beyond the data");
    ktest_assert(pbuf_put(p, pbuf_tailroom(p) + 1) == NULL, "put beyond the tail");
    ktest_assert(pbuf_put(p, pbuf_tailroom(p)) != NULL && pbuf_tailroom(p) == 0, "put to the end");
    ktest_assert(pbuf_trim(p, 50) == 0 && p->len == 50 && pbuf_trim(p, 51) == -EINVAL, "trim");
    pbuf_free(p);
}

static void test_checksum(void)
{
    /* The RFC 1071 example. */
    static const uint8_t v[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    ktest_assert(net_checksum(v, sizeof v) == 0x220d, "RFC 1071 example: %04x", net_checksum(v, sizeof v));
    /* A datagram whose (word aligned) checksum field holds the checksum
     * sums to zero, with an odd length. */
    uint8_t with[11] = { 0x45, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0, 0, 0x11 };
    uint16_t c = net_checksum(with, sizeof with);
    net_put_be16(with + 8, c);
    ktest_assert(net_checksum(with, sizeof with) == 0, "verification of an odd length sums to zero");
    /* Odd trailing byte: padded with zero, so it equals the even form. */
    uint8_t odd[3] = { 0x12, 0x34, 0x56 }, even[4] = { 0x12, 0x34, 0x56, 0x00 };
    ktest_assert(net_checksum(odd, 3) == net_checksum(even, 4), "odd length pads with zero");
    /* Pieces: even pieces then an odd tail equal one pass. */
    uint32_t sum = net_checksum_partial(with, 4, 0);
    sum = net_checksum_partial(with + 4, 4, sum);
    sum = net_checksum_partial(with + 8, 3, sum);
    ktest_assert(net_checksum_finish(sum) == 0, "accumulated pieces");
    /* Carries: all ones. */
    uint8_t ones[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    ktest_assert(net_checksum(ones, 6) == 0, "carry folding: %04x", net_checksum(ones, 6));
    ktest_assert(net_checksum(NULL, 0) == 0xffff, "empty data");
    ktest_assert(htons(0x1234) == 0x3412 && ntohl(0x78563412) == 0x12345678, "byte order");
    uint8_t b[4];
    net_put_be32(b, 0x01020304);
    ktest_assert(b[0] == 1 && b[3] == 4 && net_get_be32(b) == 0x01020304 && net_get_be16(b + 1) == 0x0203, "unaligned access");
}

/* The test's IP entry point counts and frees. While input_gate is set it
 * holds the worker inside its packet batch, which is how the tests fill
 * the queues; protocol code never waits like this. */
static volatile int input_count;
static volatile int input_gate;
static volatile uint8_t input_last_byte;

static void test_ip_input(struct netif *n, struct pbuf *p)
{
    while (__atomic_load_n(&input_gate, __ATOMIC_ACQUIRE))
        sched_yield();
    if (p->len)
        input_last_byte = p->data[p->len - 1];
    __atomic_fetch_add(&input_count, 1, __ATOMIC_RELEASE);
    pbuf_free(p);
}

static int send_loopback(uint8_t tag, size_t len)
{
    struct pbuf *p = pbuf_alloc(PBUF_DATA);
    if (!p)
        return -ENOBUFS;
    uint8_t *d = pbuf_put(p, len);
    for (size_t i = 0; i < len; i++)
        d[i] = tag;
    return netif_output(netif_loopback(), p);
}

static void wait_input(int count)
{
    for (int i = 0; i < 5000 && __atomic_load_n(&input_count, __ATOMIC_ACQUIRE) < count; i++)
        sleep_ms(1);
    ktest_assert(__atomic_load_n(&input_count, __ATOMIC_ACQUIRE) >= count, "loopback delivered %d of %d",
                 input_count, count);
}

/* Hold the worker at the gate: one packet is queued and taken by the
 * worker, which then waits in the test handler. */
static void hold_worker(void)
{
    struct net_worker_stats ws;
    net_worker_drain();
    __atomic_store_n(&input_gate, 1, __ATOMIC_RELEASE);
    ktest_assert(send_loopback(0xee, 4) == 0, "the packet that holds the worker");
    for (int i = 0; i < 5000; i++) {
        net_worker_get_stats(&ws);
        if (ws.queued_packets == 0)
            return;
        sleep_ms(1);
    }
    ktest_fail("the worker did not take the holding packet");
}

static void release_worker(void)
{
    __atomic_store_n(&input_gate, 0, __ATOMIC_RELEASE);
}

static int noop_fn(struct net_request *r)
{
    return 42;
}

static void test_loopback(void)
{
    struct netif *lo = netif_loopback();
    ktest_assert(lo && netif_find("lo") == lo && netif_is_up(lo) && lo->mtu == LOOPBACK_MTU, "lo registered and up");
    ktest_assert(netif_find("eth0") == NULL, "no other interface yet");
    uint64_t dropped_before = net_ip_input_dropped();
    ktest_assert(send_loopback(1, 20) == 0, "output on lo");
    net_worker_drain();
    ktest_assert(net_ip_input_dropped() == dropped_before + 1, "the default entry point drops and counts");
    net_set_ip_input(test_ip_input);
    input_count = 0;
    ktest_assert(send_loopback(7, LOOPBACK_MTU) == 0, "an MTU sized packet on lo");
    wait_input(1);
    ktest_assert(input_last_byte == 7, "the packet arrived intact");
    ktest_assert(atomic_u64_load_relaxed(&lo->stats.tx_packets) >= 2 && atomic_u64_load_relaxed(&lo->stats.rx_packets) >= 2,
                 "counters: tx %lu rx %lu", atomic_u64_load_relaxed(&lo->stats.tx_packets),
                 atomic_u64_load_relaxed(&lo->stats.rx_packets));
    char table[256];
    ktest_assert(netif_format_table(table, sizeof table) > 0 && strncmp(table, "1 lo up", 7) == 0, "table: %s", table);
}

static void test_queues(void)
{
    struct netif *lo = netif_loopback();
    struct net_worker_stats ws;
    struct pbuf_stats ps;
    input_count = 0;

    /* Queue exhaustion: with the worker held the input queue fills to
     * its limit; beyond it output fails, the buffer is freed and the
     * drop is counted. */
    pbuf_get_stats(&ps);
    uint32_t free_before = ps.free;
    hold_worker();                      /* one buffer is now held by the handler */
    int queued = 0, refused = 0;
    for (int i = 0; i < NET_INPUT_QUEUE_MAX + 5; i++) {
        int r = send_loopback(4, 10);
        if (r == 0)
            queued++;
        else if (r == -ENOBUFS)
            refused++;
        else
            ktest_fail("unexpected output error %d", r);
    }
    ktest_assert(queued == NET_INPUT_QUEUE_MAX && refused == 5, "queue limit: %d queued, %d refused", queued, refused);
    net_worker_get_stats(&ws);
    ktest_assert(ws.queued_packets == NET_INPUT_QUEUE_MAX && ws.packets_dropped_full >= 5,
                 "queue statistics: %u queued, %lu dropped", ws.queued_packets, ws.packets_dropped_full);
    ktest_assert(atomic_u64_load_relaxed(&lo->stats.rx_dropped) >= 5, "the interface counts the drops");
    pbuf_get_stats(&ps);
    ktest_assert(ps.free == free_before - 1 - NET_INPUT_QUEUE_MAX, "refused buffers were freed: %u free", ps.free);

    /* Requests fill up as well; a queued one cancels once and waiting
     * for it reports the cancellation. */
    static struct net_request reqs[NET_REQUEST_MAX + 3];
    int accepted = 0, rejected = 0;
    for (int i = 0; i < NET_REQUEST_MAX + 3; i++) {
        net_request_init(&reqs[i], noop_fn);
        int r = net_request_submit(&reqs[i]);
        if (r == 0)
            accepted++;
        else if (r == -ENOBUFS)
            rejected++;
    }
    ktest_assert(accepted == NET_REQUEST_MAX && rejected == 3, "request limit: %d accepted, %d rejected", accepted, rejected);
    net_worker_get_stats(&ws);
    ktest_assert(ws.requests_rejected >= 3, "rejections counted: %lu", ws.requests_rejected);
    ktest_assert(net_request_cancel(&reqs[0]) && !net_request_cancel(&reqs[0]), "a queued request cancels once");
    ktest_assert(net_request_wait(&reqs[0]) == -EINTR, "waiting for a cancelled request");

    /* Release: everything queued is processed in order, the requests
     * complete with their result, and a finished request does not cancel. */
    release_worker();
    for (int i = 1; i < NET_REQUEST_MAX; i++)
        ktest_assert(net_request_wait(&reqs[i]) == 42, "request %d completes", i);
    ktest_assert(!net_request_cancel(&reqs[1]), "a finished request does not cancel");
    wait_input(1 + NET_INPUT_QUEUE_MAX);
    net_worker_drain();
    net_worker_get_stats(&ws);
    ktest_assert(ws.queued_packets == 0 && ws.queued_requests == 0, "queues empty: %u %u", ws.queued_packets, ws.queued_requests);
    pbuf_get_stats(&ps);
    ktest_assert(ps.free == free_before, "buffers back after the batch: %u", ps.free);
}

/* Opens the gate after a while, for the paths that wait on the worker. */
static void open_gate_later(void *arg)
{
    sleep_ms(30);
    release_worker();
}

static void test_interface_down(void)
{
    struct netif *lo = netif_loopback();
    struct net_worker_stats ws;
    input_count = 0;
    hold_worker();
    for (int i = 0; i < 4; i++)
        ktest_assert(send_loopback(5, 10) == 0, "queue before down");
    uint64_t rx_dropped = atomic_u64_load_relaxed(&lo->stats.rx_dropped);
    net_worker_get_stats(&ws);
    uint64_t down_before = ws.packets_dropped_down;
    /* Taking the interface down drains the worker, which is held: a
     * helper opens the gate, and the down returns once the batch that
     * was running is over. */
    struct thread *t = thread_create("net_gate", open_gate_later, NULL, 0);
    ktest_assert(t != NULL, "gate thread");
    netif_set_up(lo, false);
    thread_join(t);
    ktest_assert(!netif_is_up(lo), "lo is down");
    net_worker_drain();
    net_worker_get_stats(&ws);
    ktest_assert(input_count == 1, "only the holding packet was delivered: %d", input_count);
    ktest_assert(ws.packets_dropped_down == down_before + 4, "queued packets of a down interface are dropped: %lu",
                 ws.packets_dropped_down - down_before);
    ktest_assert(atomic_u64_load_relaxed(&lo->stats.rx_dropped) == rx_dropped + 4, "and counted on the interface");
    uint64_t tx_dropped = atomic_u64_load_relaxed(&lo->stats.tx_dropped);
    struct pbuf_stats ps;
    pbuf_get_stats(&ps);
    uint32_t free_before = ps.free;
    ktest_assert(send_loopback(6, 10) == -ENETDOWN, "output on a down interface");
    pbuf_get_stats(&ps);
    ktest_assert(ps.free == free_before && atomic_u64_load_relaxed(&lo->stats.tx_dropped) == tx_dropped + 1,
                 "the refused buffer was freed and counted");
    netif_set_up(lo, true);
    ktest_assert(send_loopback(6, 10) == 0, "output after up");
    wait_input(2);
}

/* ---- timers ---- */

static volatile int fired_order[8];
static volatile int nfired;
static struct net_timer timers[4];
static volatile uint64_t fired_packets;
static volatile int periodic_left;

static void record_fire(struct net_timer *t)
{
    int i = __atomic_fetch_add(&nfired, 1, __ATOMIC_ACQ_REL);
    if (i < 8)
        fired_order[i] = (int)(t - timers);
    struct net_worker_stats ws;
    net_worker_get_stats(&ws);
    fired_packets = ws.packets;
}

static void periodic_fire(struct net_timer *t)
{
    __atomic_fetch_add(&nfired, 1, __ATOMIC_ACQ_REL);
    if (--periodic_left > 0)
        net_timer_arm(t, net_clock_ms() + 100);
}

static void wait_fired(int count)
{
    for (int i = 0; i < 5000 && __atomic_load_n(&nfired, __ATOMIC_ACQUIRE) < count; i++)
        sleep_ms(1);
    ktest_assert(__atomic_load_n(&nfired, __ATOMIC_ACQUIRE) == count, "%d timers fired, expected %d", nfired, count);
}

static volatile int flood_stop;
static volatile int flood_sent;

static void flood(void *arg)
{
    while (!__atomic_load_n(&flood_stop, __ATOMIC_ACQUIRE)) {
        int r = send_loopback(9, 8);
        if (r == 0)
            __atomic_fetch_add(&flood_sent, 1, __ATOMIC_RELAXED);
        else
            sched_yield();
    }
}

static void test_timers(void)
{
    struct net_worker_stats ws;
    for (int i = 0; i < 4; i++)
        net_timer_init(&timers[i], record_fire);
    net_clock_control(true);
    uint64_t now = net_clock_ms();
    nfired = 0;

    /* Armed in the future: silent, cancels once. */
    net_timer_arm(&timers[0], now + 1000);
    net_worker_drain();
    sleep_ms(20);
    ktest_assert(nfired == 0 && net_timer_armed(&timers[0]), "a future timer stays quiet");
    ktest_assert(net_timer_cancel(&timers[0]) && !net_timer_cancel(&timers[0]), "cancel once");
    net_worker_get_stats(&ws);
    ktest_assert(ws.armed_timers == 0, "no armed timer left: %u", ws.armed_timers);

    /* A deadline already reached fires at the worker's next pass with
     * no kick: arming wakes it. */
    net_timer_arm(&timers[1], now);
    wait_fired(1);
    ktest_assert(fired_order[0] == 1 && !net_timer_armed(&timers[1]) && !net_timer_cancel(&timers[1]),
                 "a due timer fires, then is neither armed nor cancellable");

    /* Three timers in reverse order fire earliest first after the clock
     * moved and the worker was kicked. */
    nfired = 0;
    net_timer_arm(&timers[0], now + 300);
    net_timer_arm(&timers[1], now + 200);
    net_timer_arm(&timers[2], now + 100);
    net_clock_advance(150);
    net_worker_kick();
    wait_fired(1);
    ktest_assert(fired_order[0] == 2, "only the earliest fired: %d", fired_order[0]);
    net_clock_advance(1000);
    net_worker_kick();
    wait_fired(3);
    ktest_assert(fired_order[1] == 1 && fired_order[2] == 0, "deadline order: %d %d %d", fired_order[0], fired_order[1], fired_order[2]);
    /* Re-arming moves a timer. */
    net_timer_arm(&timers[3], net_clock_ms() + 100);
    net_timer_arm(&timers[3], net_clock_ms() + 5000);
    net_clock_advance(200);
    net_worker_kick();
    net_worker_drain();
    ktest_assert(nfired == 3 && net_timer_armed(&timers[3]), "a moved timer keeps its new deadline");
    ktest_assert(net_timer_cancel(&timers[3]), "cancelled");

    /* A timer re-arming itself from its own function. */
    nfired = 0;
    periodic_left = 3;
    net_timer_init(&timers[3], periodic_fire);
    net_timer_arm(&timers[3], net_clock_ms());
    wait_fired(1);
    for (int i = 0; i < 2; i++) {
        net_clock_advance(100);
        net_worker_kick();
        wait_fired(2 + i);
    }
    net_worker_drain();
    ktest_assert(!net_timer_armed(&timers[3]), "the periodic timer stopped itself");

    /* Sustained input does not postpone a due timer past one batch:
     * a flood keeps the queue full while a timer becomes due. */
    net_set_ip_input(test_ip_input);
    input_count = 0;
    flood_stop = 0;
    flood_sent = 0;
    struct thread *f = thread_create("net_flood", flood, NULL, 0);
    ktest_assert(f != NULL, "flood thread");
    while (__atomic_load_n(&flood_sent, __ATOMIC_RELAXED) < 500)
        sleep_ms(1);
    nfired = 0;
    net_timer_init(&timers[0], record_fire);
    net_worker_get_stats(&ws);
    uint64_t packets_at_arm = ws.packets;
    net_timer_arm(&timers[0], net_clock_ms() + 10);
    net_clock_advance(20);
    net_worker_kick();
    wait_fired(1);
    ktest_assert(fired_packets - packets_at_arm <= 2 * NET_BATCH + 2,
                 "the timer fired after %lu packets, at most two batches", fired_packets - packets_at_arm);
    __atomic_store_n(&flood_stop, 1, __ATOMIC_RELEASE);
    thread_join(f);
    net_worker_drain();
    kprintf("net_core: flood sent %d packets\n", flood_sent);

    /* The real clock: the worker sleeps with a deadline and fires
     * without a kick. */
    net_clock_control(false);
    nfired = 0;
    net_timer_arm(&timers[0], net_clock_ms() + 30);
    uint64_t t0 = timer_ms();
    wait_fired(1);
    uint64_t took = timer_ms() - t0;
    ktest_assert(took >= 25 && took < 1000, "the real clock timer fired after %lu ms", took);
}

/* ---- requests from several threads ---- */

#define REQ_THREADS 4
#define REQ_ROUNDS 300

static void request_worker(void *arg)
{
    volatile int *failures = arg;
    for (int i = 0; i < REQ_ROUNDS; i++) {
        struct net_request r;
        net_request_init(&r, noop_fn);
        int e;
        while ((e = net_request_submit(&r)) == -ENOBUFS)
            sched_yield();
        if (e < 0 || net_request_wait(&r) != 42)
            __atomic_fetch_add(failures, 1, __ATOMIC_RELAXED);
    }
}

static void test_requests(void)
{
    /* With the controlled clock the worker sleeps without a timeout, so
     * a wakeup lost between its check and its sleep would hang the
     * requesters and the case would time out. */
    net_clock_control(true);
    volatile int failures = 0;
    struct thread *ts[REQ_THREADS];
    for (int i = 0; i < REQ_THREADS; i++) {
        ts[i] = thread_create("net_req", request_worker, (void *)&failures, 0);
        ktest_assert(ts[i] != NULL, "request thread %d", i);
    }
    for (int i = 0; i < REQ_THREADS; i++)
        thread_join(ts[i]);
    ktest_assert(failures == 0, "%d requests failed", failures);
    struct net_worker_stats ws;
    net_worker_get_stats(&ws);
    ktest_assert(ws.requests >= REQ_THREADS * REQ_ROUNDS, "requests run: %lu", ws.requests);
    net_clock_control(false);
}

static void test_cycles(void)
{
    struct pbuf_stats ps;
    struct net_worker_stats ws;
    net_set_ip_input(test_ip_input);
    net_worker_drain();
    pbuf_get_stats(&ps);
    uint32_t baseline = ps.free;
    for (int round = 0; round < 50; round++) {
        input_count = 0;
        for (int i = 0; i < 10; i++)
            ktest_assert(send_loopback((uint8_t)round, 64) == 0, "cycle %d packet %d", round, i);
        struct net_request r;
        net_request_init(&r, noop_fn);
        ktest_assert(net_request_run(&r) == 42, "cycle %d request", round);
        net_timer_init(&timers[0], record_fire);
        net_timer_arm(&timers[0], net_clock_ms() + 10000);
        ktest_assert(net_timer_cancel(&timers[0]), "cycle %d timer", round);
        wait_input(10);
    }
    net_worker_drain();
    pbuf_get_stats(&ps);
    net_worker_get_stats(&ws);
    ktest_assert(ps.free == baseline, "buffers after 50 cycles: %u, baseline %u", ps.free, baseline);
    ktest_assert(ws.queued_packets == 0 && ws.queued_requests == 0 && ws.armed_timers == 0,
                 "queues after 50 cycles: %u %u %u", ws.queued_packets, ws.queued_requests, ws.armed_timers);
    ktest_assert(ps.bad_transfer == 1, "no ownership violation beyond the deliberate one: %lu", ps.bad_transfer);
}

static void test_net_core(void)
{
    test_pbuf();
    test_checksum();
    test_loopback();
    test_queues();
    test_interface_down();
    test_timers();
    test_requests();
    test_cycles();
    net_set_ip_input(NULL);
    struct pbuf_stats ps;
    pbuf_get_stats(&ps);
    ktest_assert(ps.free == ps.total, "every buffer returned: %u of %u", ps.free, ps.total);
    kprintf("net_core: ok\n");
}
KTEST_DEFINE("net_core", test_net_core);
