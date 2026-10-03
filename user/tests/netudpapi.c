/* N05 user ABI and concurrent operations on one datagram socket. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int failures;
#define CHECK(c, text)                                                                             \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("netudpapi: FAIL %s (errno %d)\n", text, errno);                                \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)
static struct sockaddr_in address(unsigned ip, unsigned port)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(port);
    return a;
}
struct reader {
    int fd;
    ssize_t result;
    char byte;
};
static void *read_thread(void *arg)
{
    struct reader *r = arg;
    r->result = recv(r->fd, &r->byte, 1, 0);
    return NULL;
}
int main(void)
{
    int a = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int b = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    CHECK(a >= 0 && b >= 0, "create UDP sockets");
    if (a < 0 || b < 0)
        return 1;
    CHECK(fcntl(a, F_GETFD) & FD_CLOEXEC, "close-on-exec type flag");
    struct sockaddr_in aa = address(INADDR_LOOPBACK, 8800), ba = address(INADDR_LOOPBACK, 8801);
    CHECK(bind(a, (struct sockaddr *)&aa, sizeof aa) == 0, "bind a");
    CHECK(bind(b, (struct sockaddr *)&ba, sizeof ba) == 0, "bind b");
    CHECK(connect(a, (struct sockaddr *)&ba, sizeof ba) == 0, "connected UDP");
    struct sockaddr_in name = {0};
    socklen_t len = sizeof name;
    CHECK(getpeername(a, (struct sockaddr *)&name, &len) == 0 && len == sizeof name &&
              name.sin_port == ba.sin_port,
          "peer address");
    char buf[8];
    CHECK(recv(b, buf, 1, 0) == -1 && errno == EAGAIN, "nonblocking empty receive");
    CHECK(send(a, "", 0, 0) == 0, "empty datagram send");
    struct pollfd p = {
        .fd = b,
        .events = POLLIN,
    };
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN), "empty datagram poll");
    CHECK(read(b, buf, 0) == 0, "zero read request");
    CHECK(recvfrom(b, buf, sizeof buf, 0, (struct sockaddr *)&name, &len) == 0 &&
              name.sin_port == aa.sin_port,
          "empty datagram and source");
    struct iovec out[2] = {{"abc", 3}, {"def", 3}};
    struct msghdr message = {
        .msg_iov = out,
        .msg_iovlen = 2,
    };
    CHECK(sendmsg(a, &message, 0) == 6, "gather one datagram");
    CHECK(poll(&p, 1, 2000) == 1, "data arrives");
    struct {
        char first[2], guard[6];
    } small;
    memset(&small, 0x55, sizeof small);
    struct iovec in = {small.first, 2};
    message = (struct msghdr){.msg_name = &name, .msg_namelen = 2, .msg_iov = &in, .msg_iovlen = 1};
    CHECK(recvmsg(b, &message, MSG_PEEK | MSG_TRUNC) == 6 && (message.msg_flags & MSG_TRUNC),
          "peek full length");
    CHECK(message.msg_namelen == sizeof name && name.sin_family == AF_INET,
          "short source buffer reports full size");
    CHECK(small.first[0] == 'a' && small.first[1] == 'b' && small.guard[0] == 0x55 &&
              small.guard[5] == 0x55,
          "MSG_TRUNC copy remains within iovec");
    CHECK(recv(b, buf, 2, 0) == 2 && !memcmp(buf, "ab", 2),
          "receive truncates and discards remainder");
    CHECK(recv(b, buf, 8, 0) == -1 && errno == EAGAIN, "no remainder stream");
    CHECK(send(a, "x", 1, MSG_OOB) == -1 && errno == EOPNOTSUPP, "unsupported flags explicit");
    static char oversized[8192];
    CHECK(send(a, oversized, sizeof oversized, 0) == -1 && errno == EMSGSIZE,
          "oversized datagram atomic failure");
    int type = 0;
    len = sizeof type;
    CHECK(getsockopt(a, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_DGRAM,
          "socket type");
    CHECK(setsockopt(a, SOL_SOCKET, SO_REUSEADDR, &type, sizeof type) == -1 && errno == ENOPROTOOPT,
          "reuse is not silently accepted");
    struct reader reader = {
        .fd = a,
        .result = -2,
    };
    pthread_t thread;
    int created = pthread_create(&thread, NULL, read_thread, &reader);
    CHECK(created == 0, "reader thread");
    if (!created) {
        usleep(20000);
        CHECK(send(a, "q", 1, 0) == 1, "send while another thread receives");
        CHECK(poll(&p, 1, 2000) == 1 && recv(b, buf, 8, 0) == 1 && buf[0] == 'q',
              "full-duplex outbound payload");
        CHECK(sendto(b, "r", 1, 0, (struct sockaddr *)&aa, sizeof aa) == 1, "wake blocked reader");
        pthread_join(thread, NULL);
        CHECK(reader.result == 1 && reader.byte == 'r', "blocked reader completed");
    }
    CHECK(write(a, "io", 2) == 2, "write copies caller data before worker submission");
    CHECK(poll(&p, 1, 2000) == 1 && read(b, buf, sizeof buf) == 2 && !memcmp(buf, "io", 2),
          "read copies received data after releasing endpoint lock");

    /* Duplicated descriptor retains the endpoint through close. */
    int copy = dup(a);
    CHECK(copy >= 0, "duplicate");
    close(a);
    CHECK(send(copy, "s", 1, 0) == 1, "duplicate retains socket");
    CHECK(poll(&p, 1, 2000) == 1 && recv(b, buf, 8, 0) == 1 && buf[0] == 's', "duplicate payload");
    close(copy);
    close(b);
    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&aa, sizeof aa) == 0,
              "port reusable after final close");
        close(fd);
    }
    printf("netudpapi: %d failures\n", failures);
    return failures ? 1 : 0;
}
