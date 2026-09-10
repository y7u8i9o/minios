/* N01 test: the common socket layer seen from user space. A reader
 * blocked on a socket does not exclude a writer on the same descriptor,
 * message flags are honoured or rejected, addresses are copied with
 * bounds and truncation, unsupported families and protocols fail with
 * the right errors, malformed message headers fail without leaking
 * descriptors, and the socket options of the common layer work. Exits 0
 * on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ipc.h>
#include <sys/wait.h>
#include <netinet/in.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("sockapi: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

static volatile int sigpipes;
static void on_sigpipe(int sig) { sigpipes++; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* The reader blocks on fd; whatever arrives is the result. */
struct reader {
    int fd;
    char buf[32];
    ssize_t n;
    uint64_t done_at;
};

static void *blocked_reader(void *arg)
{
    struct reader *r = arg;
    r->n = read(r->fd, r->buf, sizeof r->buf - 1);
    if (r->n > 0)
        r->buf[r->n] = '\0';
    r->done_at = now_ms();
    return NULL;
}

static void test_full_duplex(void)
{
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    struct reader r = { .fd = sp[0] };
    pthread_t t;
    CHECK(pthread_create(&t, NULL, blocked_reader, &r) == 0, "reader thread");
    usleep(50000);                              /* let it block in read */
    /* A write on the same open file description while the reader
     * blocks: with a position lock held across the read this would
     * never return. */
    uint64_t t0 = now_ms();
    CHECK(write(sp[0], "ping", 4) == 4, "write on the descriptor the reader blocks on");
    uint64_t took = now_ms() - t0;
    CHECK(took < 1000, "the write did not wait for the reader: %lu ms", (unsigned long)took);
    char buf[16];
    CHECK(read(sp[1], buf, sizeof buf) == 4 && memcmp(buf, "ping", 4) == 0, "peer reads the ping");
    CHECK(write(sp[1], "pong", 4) == 4, "peer answers");
    pthread_join(t, NULL);
    CHECK(r.n == 4 && strcmp(r.buf, "pong") == 0, "reader got the answer: %ld '%s'", (long)r.n, r.buf);
    close(sp[0]);
    close(sp[1]);
}

static void test_flags(void)
{
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    char buf[64];
    errno = 0;
    CHECK(recv(sp[0], buf, sizeof buf, MSG_DONTWAIT) == -1 && errno == EAGAIN, "MSG_DONTWAIT on an empty socket");
    CHECK(send(sp[1], "hello", 5, 0) == 5, "send");
    CHECK(recv(sp[0], buf, sizeof buf, MSG_PEEK) == 5 && memcmp(buf, "hello", 5) == 0, "MSG_PEEK sees the data");
    CHECK(recv(sp[0], buf, 2, MSG_PEEK) == 2, "MSG_PEEK with a short buffer");
    CHECK(recv(sp[0], buf, sizeof buf, 0) == 5, "the peeked data is still there");
    /* MSG_WAITALL collects two writes into one read. */
    CHECK(send(sp[1], "abc", 3, 0) == 3, "first part");
    pid_t pid = fork();
    if (pid == 0) {
        usleep(50000);
        send(sp[1], "defgh", 5, 0);
        _exit(0);
    }
    CHECK(recv(sp[0], buf, 8, MSG_WAITALL) == 8 && memcmp(buf, "abcdefgh", 8) == 0, "MSG_WAITALL waits for the whole request");
    int status;
    waitpid(pid, &status, 0);
    errno = 0;
    CHECK(send(sp[1], "x", 1, MSG_OOB) == -1 && errno == EOPNOTSUPP, "MSG_OOB is rejected as unsupported");
    errno = 0;
    CHECK(recv(sp[0], buf, 1, 0x10000) == -1 && errno == EINVAL, "an unknown flag is rejected");
    errno = 0;
    CHECK(send(sp[1], "x", 1, MSG_PEEK) == -1 && errno == EINVAL, "MSG_PEEK on send is rejected");
    /* MSG_NOSIGNAL suppresses SIGPIPE; without it the signal arrives. */
    signal(SIGPIPE, on_sigpipe);
    close(sp[0]);
    errno = 0;
    CHECK(send(sp[1], "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE, "EPIPE with MSG_NOSIGNAL");
    CHECK(sigpipes == 0, "no SIGPIPE with MSG_NOSIGNAL");
    errno = 0;
    CHECK(send(sp[1], "x", 1, 0) == -1 && errno == EPIPE, "EPIPE without MSG_NOSIGNAL");
    CHECK(sigpipes == 1, "SIGPIPE without MSG_NOSIGNAL: %d", sigpipes);
    signal(SIGPIPE, SIG_IGN);
    close(sp[1]);
}

static void test_addresses(void)
{
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(ls >= 0, "socket");
    struct sockaddr_un addr = { AF_UNIX, "sockapi" };
    errno = 0;
    CHECK(bind(ls, (struct sockaddr *)&addr, 1) == -1 && errno == EINVAL, "bind with a length below the family");
    errno = 0;
    CHECK(bind(ls, (struct sockaddr *)&addr, 200) == -1 && errno == EINVAL, "bind with a length above the storage");
    struct sockaddr_in in = { AF_INET, htons(80), { htonl(INADDR_LOOPBACK) }, { 0 } };
    errno = 0;
    CHECK(bind(ls, (struct sockaddr *)&in, sizeof in) == -1 && errno == EAFNOSUPPORT, "bind with another family");
    errno = 0;
    CHECK(bind(ls, (struct sockaddr *)&addr, 2) == -1 && errno == EINVAL, "bind without a name");
    /* Unnamed: the family alone is reported. */
    struct sockaddr_storage st;
    socklen_t len = sizeof st;
    memset(&st, 0xaa, sizeof st);
    CHECK(getsockname(ls, (struct sockaddr *)&st, &len) == 0 && len == 2 && st.ss_family == AF_UNIX,
          "getsockname of an unbound socket: len %u family %u", (unsigned)len, (unsigned)st.ss_family);
    errno = 0;
    CHECK(getpeername(ls, (struct sockaddr *)&st, &len) == -1 && errno == ENOTCONN, "getpeername unconnected");
    CHECK(bind(ls, (struct sockaddr *)&addr, sizeof addr) == 0, "bind");
    len = sizeof st;
    CHECK(getsockname(ls, (struct sockaddr *)&st, &len) == 0 && len == sizeof(struct sockaddr_un) &&
          strcmp(((struct sockaddr_un *)&st)->sun_path, "sockapi") == 0, "getsockname of a bound socket");
    /* Truncation: 4 bytes of room, the full length reported. */
    struct sockaddr_un small;
    memset(&small, 0x55, sizeof small);
    len = 4;
    CHECK(getsockname(ls, (struct sockaddr *)&small, &len) == 0 && len == sizeof(struct sockaddr_un) &&
          small.sun_family == AF_UNIX && small.sun_path[0] == 's' && small.sun_path[1] == 'o' &&
          (unsigned char)small.sun_path[2] == 0x55, "getsockname truncates to the room and reports the full length");
    errno = 0;
    CHECK(getsockname(ls, NULL, &len) == -1 && errno == EFAULT, "getsockname without an address");
    CHECK(listen(ls, 4) == 0, "listen");
    pid_t pid = fork();
    if (pid == 0) {
        int cs = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(cs, (struct sockaddr *)&addr, sizeof addr) < 0)
            _exit(2);
        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        if (getpeername(cs, (struct sockaddr *)&peer, &plen) < 0 || plen != sizeof(struct sockaddr_un) ||
            strcmp(((struct sockaddr_un *)&peer)->sun_path, "sockapi") != 0)
            _exit(3);
        char c;
        read(cs, &c, 1);
        _exit(0);
    }
    struct sockaddr_storage peer;
    memset(&peer, 0xaa, sizeof peer);
    len = sizeof peer;
    int as = accept(ls, (struct sockaddr *)&peer, &len);
    CHECK(as >= 0, "accept");
    CHECK(len == 2 && peer.ss_family == AF_UNIX, "accept reports the unnamed peer: len %u", (unsigned)len);
    len = sizeof peer;
    CHECK(getsockname(as, (struct sockaddr *)&peer, &len) == 0 && len == sizeof(struct sockaddr_un) &&
          strcmp(((struct sockaddr_un *)&peer)->sun_path, "sockapi") == 0, "the accepted socket carries the listener's name");
    errno = 0;
    CHECK(accept(ls, (struct sockaddr *)&peer, NULL) == -1 && errno == EFAULT, "accept with an address and no length");
    /* sendto with an address on a connected stream. */
    errno = 0;
    CHECK(sendto(as, "x", 1, 0, (struct sockaddr *)&addr, sizeof addr) == -1 && errno == EISCONN, "sendto on a connected stream");
    CHECK(write(as, "x", 1) == 1, "release the child");
    int status;
    waitpid(pid, &status, 0);
    CHECK(status == 0, "child status 0x%x", status);
    /* recvfrom fills the (unnamed) source. */
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    CHECK(write(sp[1], "y", 1) == 1, "write");
    char c;
    len = sizeof peer;
    memset(&peer, 0xaa, sizeof peer);
    CHECK(recvfrom(sp[0], &c, 1, 0, (struct sockaddr *)&peer, &len) == 1 && len == 2 && peer.ss_family == AF_UNIX,
          "recvfrom reports the source: len %u", (unsigned)len);
    close(sp[0]);
    close(sp[1]);
    close(as);
    close(ls);
}

static void test_families(void)
{
    errno = 0;
    CHECK(socket(77, SOCK_STREAM, 0) == -1 && errno == EAFNOSUPPORT, "unknown family");
    errno = 0;
    CHECK(socket(AF_INET, SOCK_STREAM, 0) == -1 && errno == EPROTONOSUPPORT, "AF_INET stream is not supported yet");
    errno = 0;
    CHECK(socket(AF_INET, SOCK_DGRAM, 0) == -1 && errno == EPROTONOSUPPORT, "AF_INET datagram is not supported yet");
    errno = 0;
    CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_UDP) == -1 && errno == EPROTONOSUPPORT, "stream over UDP");
    errno = 0;
    CHECK(socket(AF_INET, SOCK_RAW, 0) == -1 && errno == ESOCKTNOSUPPORT, "raw sockets are not supported");
    errno = 0;
    CHECK(socket(AF_INET, 9, 0) == -1 && errno == ESOCKTNOSUPPORT, "unknown type");
    errno = 0;
    CHECK(socket(AF_UNIX, SOCK_DGRAM, 0) == -1 && errno == ESOCKTNOSUPPORT, "Unix datagrams are not supported");
    errno = 0;
    CHECK(socket(AF_UNIX, SOCK_STREAM, 7) == -1 && errno == EPROTONOSUPPORT, "a Unix protocol number that is not a flag");
    /* The legacy flag argument of AF_UNIX and the flags in the type. */
    int a = socket(AF_UNIX, SOCK_STREAM, SOCK_NONBLOCK);
    CHECK(a >= 0 && (fcntl(a, F_GETFL) & O_NONBLOCK), "AF_UNIX flags in the third argument");
    int b = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    CHECK(b >= 0 && (fcntl(b, F_GETFL) & O_NONBLOCK) && (fcntl(b, F_GETFD) & FD_CLOEXEC), "flags in the type");
    close(a);
    close(b);
}

static void test_iovecs(void)
{
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    char data[8] = "12345678";
    struct iovec big[2] = { { data, (size_t)-1 }, { data, (size_t)-1 } };
    struct msghdr m = { NULL, 0, big, 2, NULL, 0, 0 };
    errno = 0;
    CHECK(sendmsg(sp[0], &m, 0) == -1 && errno == EINVAL, "iovec lengths that overflow");
    struct iovec many[40];
    for (int i = 0; i < 40; i++)
        many[i] = (struct iovec){ data, 1 };
    m = (struct msghdr){ NULL, 0, many, 40, NULL, 0, 0 };
    errno = 0;
    CHECK(sendmsg(sp[0], &m, 0) == -1 && errno == EINVAL, "too many iovecs");
    struct iovec bad = { (void *)0x10, 16 };
    m = (struct msghdr){ NULL, 0, &bad, 1, NULL, 0, 0 };
    errno = 0;
    CHECK(sendmsg(sp[0], &m, 0) == -1 && errno == EFAULT, "an unmapped iovec");
    errno = 0;
    CHECK(recvmsg(sp[0], &m, MSG_DONTWAIT) == -1 && errno == EFAULT, "an unmapped receive iovec");
    /* A malformed control message must not leave a descriptor behind:
     * the next descriptor number stays the same. */
    int probe = dup(sp[0]);
    close(probe);
    struct iovec iov = { data, 4 };
    char ctl[CMSG_SPACE(sizeof(int) * 2)];
    memset(ctl, 0, sizeof ctl);
    struct cmsghdr *c = (struct cmsghdr *)ctl;
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * 2) + 100;        /* longer than the buffer */
    memcpy(CMSG_DATA(c), (int[]){ sp[1], sp[1] }, sizeof(int) * 2);
    m = (struct msghdr){ NULL, 0, &iov, 1, ctl, sizeof ctl, 0 };
    errno = 0;
    CHECK(sendmsg(sp[0], &m, 0) == -1 && errno == EINVAL, "a control record longer than the buffer");
    c->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    memcpy(CMSG_DATA(c), (int[]){ sp[1], 200 }, sizeof(int) * 2);
    errno = 0;
    CHECK(sendmsg(sp[0], &m, 0) == -1 && errno == EBADF, "a bad descriptor in the control record");
    int again = dup(sp[0]);
    CHECK(again == probe, "no descriptor leaked by the failed sendmsg: %d vs %d", again, probe);
    close(again);
    /* Streams accept more than the per-call limit and write partially. */
    size_t huge = 100000;
    char *bigbuf = malloc(huge);
    memset(bigbuf, 'z', huge);
    iov = (struct iovec){ bigbuf, huge };
    m = (struct msghdr){ NULL, 0, &iov, 1, NULL, 0, 0 };
    ssize_t n = sendmsg(sp[0], &m, MSG_DONTWAIT);
    CHECK(n > 0 && n <= 65536, "a stream sendmsg above the call limit writes part: %ld", (long)n);
    free(bigbuf);
    close(sp[0]);
    close(sp[1]);
}

static void test_options(void)
{
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    int v = -1;
    socklen_t len = sizeof v;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_TYPE, &v, &len) == 0 && v == SOCK_STREAM && len == sizeof v, "SO_TYPE");
    len = sizeof v;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_DOMAIN, &v, &len) == 0 && v == AF_UNIX, "SO_DOMAIN");
    len = sizeof v;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_PROTOCOL, &v, &len) == 0 && v == 0, "SO_PROTOCOL");
    len = sizeof v;
    v = -1;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_ERROR, &v, &len) == 0 && v == 0, "SO_ERROR is clear");
    len = 1;
    errno = 0;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_TYPE, &v, &len) == -1 && errno == EINVAL, "SO_TYPE with too little room");
    errno = 0;
    CHECK(setsockopt(sp[0], SOL_SOCKET, SO_TYPE, &v, sizeof v) == -1 && errno == ENOPROTOOPT, "SO_TYPE is read only");
    errno = 0;
    CHECK(setsockopt(sp[0], SOL_SOCKET, SO_REUSEADDR, &v, sizeof v) == -1 && errno == ENOPROTOOPT, "SO_REUSEADDR on a Unix socket");
    errno = 0;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_TYPE, &v, NULL) == -1 && errno == EFAULT, "getsockopt without a length");
    errno = 0;
    CHECK(setsockopt(sp[0], SOL_SOCKET, SO_REUSEADDR, &v, 1000) == -1 && errno == EINVAL, "setsockopt with an oversized value");
    int pfd[2];
    CHECK(pipe(pfd) == 0, "pipe");
    errno = 0;
    CHECK(getsockopt(pfd[0], SOL_SOCKET, SO_TYPE, &v, &len) == -1 && errno == ENOTSOCK, "getsockopt on a pipe");
    close(pfd[0]);
    close(pfd[1]);
    close(sp[0]);
    close(sp[1]);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_full_duplex();
    test_flags();
    test_addresses();
    test_families();
    test_iovecs();
    test_options();
    printf("sockapi: %d failures\n", failures);
    return failures ? 1 : 0;
}
