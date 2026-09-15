/* netpeer: the controlled host peer of the network boot tests.
 *
 * QEMU's dgram network backend sends every Ethernet frame the guest
 * transmits as one UDP datagram to a remote address and hands every
 * datagram received on its local port to the guest as a frame. netpeer is
 * the process at the other end. It binds a UDP socket on 127.0.0.1, picks
 * a free port for QEMU's local side, publishes both in the ready file
 * ("<peer port> <guest port>"), and then serves frames until it is
 * terminated, writing one line per frame and a final summary to the log.
 *
 * usage: netpeer --ready FILE --log FILE [--pid FILE] [--mode MODE]
 *        netpeer --probe PORT
 *
 * Modes: count (default) records the frames it receives; echo sends every
 * frame back unchanged. --probe exits 0 when UDP port PORT on 127.0.0.1
 * is free and 1 when it is still bound, which the harness self test uses
 * to verify that a terminated peer released its port.
 *
 * Mode ip answers ARP and ICMP on an isolated Ethernet link. Modes udp and tcp
 * are native host echo sockets for the QEMU user backend. Both use
 * the same readiness and cleanup lifecycle as count and raw echo. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

static void die(const char *what)
{
    fprintf(stderr, "netpeer: %s: %s\n", what, strerror(errno));
    exit(2);
}

static int bind_socket(int type, unsigned short port, unsigned short *bound)
{
    int fd = socket(AF_INET, type, 0);
    if (fd < 0)
        die("socket");
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    socklen_t len = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &len) < 0)
        die("getsockname");
    *bound = ntohs(sa.sin_port);
    return fd;
}

static int bind_udp(unsigned short port, unsigned short *bound)
{
    return bind_socket(SOCK_DGRAM, port, bound);
}

/* Native TCP exercises an independent host stack. Wait for the guest's
 * half-close before replying so EOF ordering is part of every exchange. */
static void serve_tcp(int listener, FILE *log, int streaming)
{
    unsigned connections = 0;
    while (!stopping) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            die("TCP accept");
        }
        if (streaming) {
            unsigned char block[4096];
            size_t total = 0;
            while (!stopping) {
                ssize_t length = recv(fd, block, sizeof block, 0);
                if (length < 0 && errno == EINTR)
                    continue;
                if (length <= 0)
                    break;
                size_t sent = 0;
                while (!stopping && sent < (size_t)length) {
                    ssize_t result = send(fd, block + sent, (size_t)length - sent, 0);
                    if (result < 0 && errno == EINTR)
                        continue;
                    if (result <= 0)
                        break;
                    sent += (size_t)result;
                }
                if (sent != (size_t)length)
                    break;
                total += sent;
            }
            shutdown(fd, SHUT_WR);
            close(fd);
            fprintf(log, "tcp stream %u bytes %zu\n", ++connections, total);
            continue;
        }
        unsigned char data[512];
        size_t received = 0;
        int eof = 0;
        while (!stopping && received < sizeof data) {
            ssize_t result = recv(fd, data + received, sizeof data - received, 0);
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0) {
                eof = result == 0;
                break;
            }
            received += (size_t)result;
        }
        size_t sent = 0;
        while (!stopping && eof && sent < received) {
            ssize_t result = send(fd, data + sent, received - sent, 0);
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                break;
            sent += (size_t)result;
        }
        shutdown(fd, SHUT_WR);
        close(fd);
        fprintf(log,
                "tcp connection %u received %zu eof %d sent %zu\n",
                ++connections,
                received,
                eof,
                sent);
    }
    fprintf(log, "tcp summary connections %u\n", connections);
}

/* Write the whole file under a temporary name, then rename it, so a
 * reader never sees a partial line. */
static void publish(const char *path, unsigned short peer, unsigned short guest)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        die("ready file");
    fprintf(f, "%u %u\n", peer, guest);
    fclose(f);
    if (rename(tmp, path) < 0)
        die("rename ready file");
}

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

/* An independent wire peer, deliberately separate from the guest parsers.
 * It owns 10.0.0.1 and responds as a gateway for the isolated test link. */
static unsigned short get16(const unsigned char *p)
{
    return (unsigned short)((p[0] << 8) | p[1]);
}
static void put16(unsigned char *p, unsigned short n)
{
    p[0] = n >> 8;
    p[1] = n;
}
static unsigned short checksum(const unsigned char *p, size_t n)
{
    unsigned sum = 0;
    while (n > 1) {
        sum += get16(p);
        p += 2;
        n -= 2;
    }
    if (n)
        sum += (unsigned)*p << 8;
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    return (unsigned short)~sum;
}
static size_t reply_frame(unsigned char *b, size_t n, FILE *log)
{
    static const unsigned char mac[6] = {2, 0, 0, 0, 0, 1};
    if (n < 42)
        return 0;
    if (get16(b + 12) == 0x806 && get16(b + 20) == 1 && b[38] == 10 && b[41] == 1) {
        unsigned char sender[10];
        memcpy(sender, b + 22, 10);
        memcpy(b, b + 6, 6);
        memcpy(b + 6, mac, 6);
        put16(b + 20, 2);
        memcpy(b + 22, mac, 6);
        memcpy(b + 28, b + 38, 4);
        memcpy(b + 32, sender, 10);
        fprintf(log, "arp reply\n");
        return 42;
    }
    if (get16(b + 12) != 0x800 || b[14] != 0x45 || checksum(b + 14, 20))
        return 0;
    size_t total = get16(b + 16);
    if (total < 28 || total + 14 > n || b[23] != 1 || checksum(b + 34, total - 20))
        return 0;
    if (b[34] == 0) {
        fprintf(log, "icmp guest reply\n");
        return 0;
    }
    if (b[34] != 8 || b[35])
        return 0;
    memcpy(b, b + 6, 6);
    memcpy(b + 6, mac, 6);
    unsigned char address[4];
    memcpy(address, b + 26, 4);
    memcpy(b + 26, b + 30, 4);
    memcpy(b + 30, address, 4);
    b[34] = 0;
    b[36] = b[37] = 0;
    put16(b + 36, checksum(b + 34, total - 20));
    b[24] = b[25] = 0;
    put16(b + 24, checksum(b + 14, 20));
    fprintf(log, "icmp peer reply\n");
    return total + 14;
}

int main(int argc, char **argv)
{
    const char *ready = NULL, *log = NULL, *pidfile = NULL, *mode = "count";
    long probe = -1;
    int probe_type = SOCK_DGRAM;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ready") == 0 && i + 1 < argc)
            ready = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log = argv[++i];
        else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc)
            pidfile = argv[++i];
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            mode = argv[++i];
        else if (strcmp(argv[i], "--probe") == 0 && i + 1 < argc)
            probe = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--probe-tcp") == 0 && i + 1 < argc) {
            probe = strtol(argv[++i], NULL, 10);
            probe_type = SOCK_STREAM;
        } else {
            fprintf(stderr,
                    "usage: netpeer --ready FILE --log FILE [--pid FILE] [--mode "
                    "count|echo|ip|udp|tcp|tcp-stream]\n"
                    "       netpeer --probe PORT | --probe-tcp PORT\n");
            return 2;
        }
    }
    if (probe >= 0) {
        unsigned short bound;
        int fd = bind_socket(probe_type, (unsigned short)probe, &bound);
        if (fd < 0)
            return 1;
        close(fd);
        return 0;
    }
    if (!ready || !log) {
        fprintf(stderr, "netpeer: --ready and --log are required\n");
        return 2;
    }
    if (strcmp(mode, "count") != 0 && strcmp(mode, "echo") != 0 && strcmp(mode, "ip") != 0 &&
        strcmp(mode, "udp") != 0 && strcmp(mode, "tcp") != 0 && strcmp(mode, "tcp-stream") != 0) {
        fprintf(stderr, "netpeer: unknown mode %s\n", mode);
        return 2;
    }

    /* Without SA_RESTART the blocking receive returns EINTR on a
     * signal, which is how the loop below notices the stop request. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
    unsigned short peer_port, guest_port;
    int fd = bind_socket(strncmp(mode, "tcp", 3) == 0 ? SOCK_STREAM : SOCK_DGRAM, 0, &peer_port);
    if (fd < 0)
        die("bind");
    if (strncmp(mode, "tcp", 3) == 0 && listen(fd, 8) < 0)
        die("TCP listen");
    /* A port for QEMU's side: bound once to find it free, released for
     * QEMU to bind. Ports are handed out increasing on the hosts in use,
     * so the window between release and QEMU's bind is not a problem in
     * practice; a collision fails QEMU's start, which the harness reports. */
    int probe_fd = bind_udp(0, &guest_port);
    if (probe_fd < 0)
        die("bind guest port");
    close(probe_fd);

    FILE *lf = fopen(log, "w");
    if (!lf)
        die("log file");
    setvbuf(lf, NULL, _IOLBF, 0);
    if (pidfile) {
        FILE *pf = fopen(pidfile, "w");
        if (!pf)
            die("pid file");
        fprintf(pf, "%ld\n", (long)getpid());
        fclose(pf);
    }
    double start = now_s();
    fprintf(lf,
            "netpeer pid %ld mode %s peer 127.0.0.1:%u guest 127.0.0.1:%u\n",
            (long)getpid(),
            mode,
            peer_port,
            guest_port);
    publish(ready, peer_port, guest_port);

    if (strncmp(mode, "tcp", 3) == 0) {
        serve_tcp(fd, lf, strcmp(mode, "tcp-stream") == 0);
        fclose(lf);
        close(fd);
        if (pidfile)
            unlink(pidfile);
        return 0;
    }

    struct sockaddr_in guest;
    memset(&guest, 0, sizeof guest);
    guest.sin_family = AF_INET;
    guest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    guest.sin_port = htons(guest_port);

    unsigned long frames = 0, bytes = 0, echoed = 0;
    int sent_echo_request = 0;
    unsigned char buf[65536];
    while (!stopping) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("recvfrom");
        }
        frames++;
        bytes += (unsigned long)n;
        fprintf(lf, "%.3f frame %lu len %zd", now_s() - start, frames, n);
        for (ssize_t i = 0; i < n && i < 14; i++)
            fprintf(lf, "%s%02x", i ? (i == 6 || i == 12 ? " " : ":") : " ", buf[i]);
        fputc('\n', lf);
        if (strcmp(mode, "udp") == 0) {
            if (sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&from, fromlen) == n)
                echoed++;
        } else if (strcmp(mode, "ip") == 0) {
            size_t length = reply_frame(buf, (size_t)n, lf);
            if (length && sendto(fd, buf, length, 0, (struct sockaddr *)&guest, sizeof guest) ==
                              (ssize_t)length)
                echoed++;
            /* After one echo reply, inject a valid host-originated request.
             * Reuse the independently checksummed reply and its addresses. */
            if (length && get16(buf + 12) == 0x800 && !sent_echo_request) {
                sent_echo_request = 1;
                buf[34] = 8;
                buf[36] = buf[37] = 0;
                put16(buf + 36, checksum(buf + 34, length - 34));
                sendto(fd, buf, length, 0, (struct sockaddr *)&guest, sizeof guest);
                fprintf(lf, "icmp peer request\n");
            }
        } else if (strcmp(mode, "echo") == 0) {
            if (sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&guest, sizeof guest) == n)
                echoed++;
            else
                fprintf(lf, "echo failed: %s\n", strerror(errno));
        }
    }
    fprintf(lf,
            "summary frames %lu bytes %lu echoed %lu seconds %.3f\n",
            frames,
            bytes,
            echoed,
            now_s() - start);
    fclose(lf);
    close(fd);
    if (pidfile)
        unlink(pidfile);
    return 0;
}
