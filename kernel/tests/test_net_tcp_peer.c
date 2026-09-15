/* TCP through VirtIO and QEMU's user backend to a native host TCP socket. */
#include <console.h>
#include <errno.h>
#include <drivers/timer.h>
#include <net/tcp.h>
#include <lib/cmdline.h>
#include "net_helpers.h"

static bool wait_readiness(struct socket *socket, int events)
{
    uint64_t deadline = timer_ms() + 5000;
    do {
        if (socket->ops->poll(socket) & events)
            return true;
        sleep_ms(1);
    } while (timer_ms() < deadline);
    return false;
}
static void test_tcp_peer(void)
{
    char port_text[16];
    ktest_assert(cmdline_lookup("netpeer_port", port_text, sizeof port_text), "host port supplied");
    unsigned port = 0;
    for (char *digit = port_text; *digit; digit++) {
        ktest_assert(*digit >= '0' && *digit <= '9', "numeric host port");
        port = port * 10 + *digit - '0';
    }
    ktest_assert(port && port <= 65535, "valid host port");
    struct netif *interface = netif_find("eth0");
    ktest_assert(interface && net_configure(interface, 0x0a00020f, 0xffffff00u, 0x0a000202) == 0,
                 "configure QEMU user network");
    for (unsigned round = 0; round < 20; round++) {
        struct file *file = NULL;
        ktest_assert(socket_create(AF_INET, SOCK_STREAM, 0, O_NONBLOCK, &file) == 0,
                     "native TCP socket %u",
                     round);
        struct socket *socket = socket_from_file(file);
        struct sockaddr_storage name;
        address(&name, 0x0a000202, port);
        ktest_assert(socket_connect(socket, &name, 16) == -EINPROGRESS, "start native connect");
        ktest_assert(wait_readiness(socket, POLLOUT), "native connect completes");
        int error = -1;
        socklen_t error_length = sizeof error;
        ktest_assert(socket_getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 &&
                         !error,
                     "native connect SO_ERROR %d",
                     error);
        char outgoing[257], incoming[257];
        size_t length = round % 2 ? sizeof outgoing : 37;
        for (size_t i = 0; i < length; i++)
            outgoing[i] = (char)(round + i);
        struct socket_msg message = {
            .data = outgoing,
            .len = length,
            .flags = MSG_NOSIGNAL,
        };
        ktest_assert(socket_sendmsg(socket, &message) == (long)length, "native send %u", round);
        ktest_assert(socket_shutdown(socket, SHUT_WR) == 0, "native half-close");
        size_t received = 0;
        while (received < length) {
            ktest_assert(wait_readiness(socket, POLLIN), "native response arrives");
            message.data = incoming + received;
            message.len = length - received;
            message.flags = 0;
            long result = socket_recvmsg(socket, &message);
            ktest_assert(result > 0, "native response %u result %ld", round, result);
            received += (size_t)result;
        }
        ktest_assert(!memcmp(outgoing, incoming, length), "native response payload %u", round);
        ktest_assert(wait_readiness(socket, POLLIN), "native FIN arrives");
        message.len = 1;
        ktest_assert(socket_recvmsg(socket, &message) == 0, "native EOF after response");
        file_put(file);
    }
    kprintf("net_tcp_peer: 20 native host connections and half-closes, ok\n");
}
KTEST_DEFINE("net_tcp_peer", test_tcp_peer);

static void test_tcp_bulk(void)
{
    char port_text[16];
    ktest_assert(cmdline_lookup("netpeer_port", port_text, sizeof port_text), "bulk peer port");
    unsigned port = 0;
    for (const char *digit = port_text; *digit; digit++)
        port = port * 10 + *digit - '0';
    struct netif *interface = netif_find("eth0");
    ktest_assert(interface && net_configure(interface, 0x0a00020f, 0xffffff00u, 0x0a000202) == 0,
                 "bulk native path");
    struct file *file;
    ktest_assert(socket_create(AF_INET, SOCK_STREAM, 0, O_NONBLOCK, &file) == 0, "bulk socket");
    struct socket *socket = socket_from_file(file);
    struct sockaddr_storage name;
    address(&name, 0x0a000202, port);
    ktest_assert(socket_connect(socket, &name, 16) == -EINPROGRESS, "bulk connect");
    ktest_assert(wait_readiness(socket, POLLOUT), "bulk connected");
    int error = -1;
    socklen_t error_length = sizeof error;
    ktest_assert(socket_getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 &&
                     !error,
                 "bulk connection error");
    enum { TOTAL = 262144 };
    static char outgoing[16384], incoming[4096];
    size_t sent = 0, received = 0;
    bool closed = false, eof = false;
    uint64_t started = timer_ms();
    uint64_t deadline = started + 60000;
    while (!eof && timer_ms() < deadline) {
        if (sent < TOTAL) {
            size_t length = MIN(sizeof outgoing, TOTAL - sent);
            for (size_t i = 0; i < length; i++)
                outgoing[i] = (char)((sent + i) * 37 + ((sent + i) >> 8));
            struct socket_msg message = {
                .data = outgoing,
                .len = length,
                .flags = MSG_NOSIGNAL,
            };
            long result = socket_sendmsg(socket, &message);
            ktest_assert(result > 0 || result == -EAGAIN, "bulk send %ld", result);
            if (result > 0)
                sent += result;
        } else if (!closed) {
            ktest_assert(socket_shutdown(socket, SHUT_WR) == 0, "bulk half-close");
            closed = true;
        }
        struct socket_msg message = {
            .data = incoming,
            .len = sizeof incoming,
        };
        long result = socket_recvmsg(socket, &message);
        ktest_assert(result >= 0 || result == -EAGAIN, "bulk receive %ld", result);
        if (result > 0) {
            for (long i = 0; i < result; i++)
                ktest_assert(incoming[i] == (char)((received + i) * 37 + ((received + i) >> 8)),
                             "bulk stream mismatch at %lu",
                             received + i);
            received += result;
        } else if (!result) {
            eof = true;
        } else {
            sleep_ms(1);
        }
    }
    ktest_assert(sent == TOTAL && received == TOTAL && eof,
                 "bulk transfer totals %lu/%lu EOF %d",
                 sent,
                 received,
                 eof);
    file_put(file);
    uint64_t elapsed = MAX(timer_ms() - started, 1u);
    struct tcp_stats stats;
    tcp_get_stats(&stats);
    kprintf("net_tcp_bulk: 262144 bytes each direction, ok\n");
    kprintf("net_tcp_bulk: %lu ms, %lu KiB/s per direction, %lu retransmits\n",
            (unsigned long)elapsed, (unsigned long)(TOTAL / elapsed * 1000 / 1024),
            (unsigned long)stats.retransmits);
}
KTEST_DEFINE("net_tcp_bulk", test_tcp_bulk);
