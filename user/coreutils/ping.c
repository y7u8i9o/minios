/* ping: ICMP echo through the narrow /dev/net interface (N11). No raw
 * sockets are involved; the kernel matches replies to its own requests.
 *   ping [-c COUNT] [-s SIZE] [-W TIMEOUT_MS] HOST */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <minios/abi.h>

int main(int argc, char **argv)
{
    int count = 4, size = 56, timeout = 2000;
    int opt;
    while ((opt = getopt(argc, argv, "c:s:W:")) != -1) {
        if (opt == 'c')
            count = atoi(optarg);
        else if (opt == 's')
            size = atoi(optarg);
        else if (opt == 'W')
            timeout = atoi(optarg);
        else
            return fprintf(stderr, "usage: ping [-c COUNT] [-s SIZE] [-W MS] HOST\n"), 2;
    }
    if (optind != argc - 1)
        return fprintf(stderr, "usage: ping [-c COUNT] [-s SIZE] [-W MS] HOST\n"), 2;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *ai;
    int error = getaddrinfo(argv[optind], NULL, &hints, &ai);
    if (error)
        return fprintf(stderr, "ping: %s: %s\n", argv[optind], gai_strerror(error)), 1;
    struct sockaddr_in *sin = (struct sockaddr_in *)ai->ai_addr;
    char text[16];
    inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text);
    uint32_t address = ntohl(sin->sin_addr.s_addr);
    freeaddrinfo(ai);
    int fd = open("/dev/net", O_RDONLY);
    if (fd < 0)
        return perror("ping: /dev/net"), 1;
    printf("PING %s (%s): %d data bytes\n", argv[optind], text, size);
    int received = 0;
    for (int i = 0; i < count; i++) {
        struct net_ping p = {.address = address, .timeout_ms = (uint32_t)timeout,
                             .sequence = (uint16_t)i, .size = (uint16_t)size};
        if (ioctl(fd, NETIOC_PING, &p) == 0) {
            received++;
            printf("%d bytes from %s: seq=%d ttl=%u time=%u ms\n", size + 8, text, i, p.ttl,
                   p.rtt_ms);
        } else {
            printf("seq=%d: %s\n", i, strerror(errno));
        }
        fflush(stdout);
        if (i + 1 < count)
            sleep(1);
    }
    printf("--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss\n",
           argv[optind], count, received, count ? (count - received) * 100 / count : 0);
    close(fd);
    return received ? 0 : 1;
}
