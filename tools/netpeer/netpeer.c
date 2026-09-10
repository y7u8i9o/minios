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
 * The tool keeps no protocol knowledge: later milestones add responder
 * modes (ARP, ICMP echo, UDP and TCP peers) here so that the harness
 * lifecycle stays the same. */
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

static int bind_udp(unsigned short port, unsigned short *bound)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
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

int main(int argc, char **argv)
{
    const char *ready = NULL, *log = NULL, *pidfile = NULL, *mode = "count";
    long probe = -1;
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
        else {
            fprintf(stderr, "usage: netpeer --ready FILE --log FILE [--pid FILE] [--mode count|echo]\n"
                            "       netpeer --probe PORT\n");
            return 2;
        }
    }
    if (probe >= 0) {
        unsigned short bound;
        int fd = bind_udp((unsigned short)probe, &bound);
        if (fd < 0)
            return 1;
        close(fd);
        return 0;
    }
    if (!ready || !log) {
        fprintf(stderr, "netpeer: --ready and --log are required\n");
        return 2;
    }
    if (strcmp(mode, "count") != 0 && strcmp(mode, "echo") != 0) {
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

    unsigned short peer_port, guest_port;
    int fd = bind_udp(0, &peer_port);
    if (fd < 0)
        die("bind");
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
    fprintf(lf, "netpeer pid %ld mode %s peer 127.0.0.1:%u guest 127.0.0.1:%u\n",
            (long)getpid(), mode, peer_port, guest_port);
    publish(ready, peer_port, guest_port);

    struct sockaddr_in guest;
    memset(&guest, 0, sizeof guest);
    guest.sin_family = AF_INET;
    guest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    guest.sin_port = htons(guest_port);

    unsigned long frames = 0, bytes = 0, echoed = 0;
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
        if (strcmp(mode, "echo") == 0) {
            if (sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&guest, sizeof guest) == n)
                echoed++;
            else
                fprintf(lf, "echo failed: %s\n", strerror(errno));
        }
    }
    fprintf(lf, "summary frames %lu bytes %lu echoed %lu seconds %.3f\n", frames, bytes, echoed,
            now_s() - start);
    fclose(lf);
    close(fd);
    if (pidfile)
        unlink(pidfile);
    return 0;
}
