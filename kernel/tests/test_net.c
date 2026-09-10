/* Network stack self tests (NETWORK_PLAN.md): the controlled clock (N00),
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
    ktest_assert(socket_create(AF_INET, SOCK_STREAM, 0, 0, &f) == -EPROTONOSUPPORT, "AF_INET stream pending");
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
