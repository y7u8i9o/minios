/* nc: a small TCP/UDP client and server (N11). Standard input goes to the
 * peer and the peer's data to standard output until both sides finish.
 *   nc [-u] HOST PORT        connect
 *   nc [-u] -l PORT          listen for one connection (or one UDP peer) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

static int usage(void)
{
    fprintf(stderr, "usage: nc [-u] HOST PORT | nc [-u] -l PORT\n");
    return 2;
}

/* Relay stdin to the socket and the socket to stdout. For UDP a peer
 * address is learned from the first datagram when none is known. */
static int relay(int fd, int udp, struct sockaddr_in *peer)
{
    int have_peer = peer->sin_port != 0;
    int stdin_open = 1;
    for (;;) {
        struct pollfd pfd[2] = {{.fd = fd, .events = POLLIN}, {.fd = 0, .events = POLLIN}};
        if (poll(pfd, stdin_open ? 2 : 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        char buf[4096];
        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            struct sockaddr_in from;
            socklen_t len = sizeof from;
            ssize_t n = udp ? recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &len)
                            : read(fd, buf, sizeof buf);
            if (n < 0) {
                fprintf(stderr, "nc: %s\n", strerror(errno));
                return 1;
            }
            if (n == 0 && !udp)
                return 0;
            if (udp && !have_peer) {
                *peer = from;
                have_peer = 1;
            }
            if (write(1, buf, (size_t)n) != n)
                return 1;
        }
        if (stdin_open && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) {
                stdin_open = 0;
                if (!udp)
                    shutdown(fd, SHUT_WR);
                continue;
            }
            ssize_t sent = udp ? (have_peer ? sendto(fd, buf, (size_t)n, 0,
                                                     (struct sockaddr *)peer, sizeof *peer)
                                            : n)
                               : send(fd, buf, (size_t)n, MSG_NOSIGNAL);
            if (sent != n) {
                fprintf(stderr, "nc: send: %s\n", strerror(errno));
                return 1;
            }
        }
    }
}

int main(int argc, char **argv)
{
    int udp = 0, listen_mode = 0, opt;
    while ((opt = getopt(argc, argv, "ul")) != -1) {
        if (opt == 'u')
            udp = 1;
        else if (opt == 'l')
            listen_mode = 1;
        else
            return usage();
    }
    int rest = argc - optind;
    if ((listen_mode && rest != 1) || (!listen_mode && rest != 2))
        return usage();
    const char *host = listen_mode ? NULL : argv[optind];
    const char *port = argv[argc - 1];
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM,
                             .ai_flags = listen_mode ? AI_PASSIVE : 0}, *ai;
    int error = getaddrinfo(host, port, &hints, &ai);
    if (error)
        return fprintf(stderr, "nc: %s: %s\n", host ? host : port, gai_strerror(error)), 1;
    int fd = socket(AF_INET, hints.ai_socktype, 0);
    if (fd < 0)
        return perror("nc: socket"), 1;
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof peer);
    int status;
    if (listen_mode) {
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0)
            return fprintf(stderr, "nc: bind: %s\n", strerror(errno)), 1;
        if (udp) {
            status = relay(fd, 1, &peer);
        } else {
            if (listen(fd, 1) < 0)
                return perror("nc: listen"), 1;
            socklen_t len = sizeof peer;
            int conn = accept(fd, (struct sockaddr *)&peer, &len);
            if (conn < 0)
                return perror("nc: accept"), 1;
            status = relay(conn, 0, &peer);
            close(conn);
        }
    } else {
        memcpy(&peer, ai->ai_addr, sizeof peer);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0)
            return fprintf(stderr, "nc: connect %s:%s: %s\n", host, port, strerror(errno)), 1;
        status = relay(fd, udp, &peer);
    }
    freeaddrinfo(ai);
    close(fd);
    return status;
}
