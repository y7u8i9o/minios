/* M23: Unix domain sockets, descriptor passing, non blocking mode. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("socktest: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

static int send_with_fds(int sock, const char *text, int *fds, int nfds)
{
    struct iovec iov = { (void *)text, strlen(text) };
    char ctl[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr m = { NULL, 0, &iov, 1, ctl, CMSG_SPACE(sizeof(int) * (size_t)nfds), 0 };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    return (int)sendmsg(sock, &m, 0);
}

static int recv_with_fds(int sock, char *buf, size_t size, int *fds, int max)
{
    struct iovec iov = { buf, size - 1 };
    char ctl[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr m = { NULL, 0, &iov, 1, ctl, sizeof ctl, 0 };
    int n = (int)recvmsg(sock, &m, 0);
    if (n < 0)
        return n;
    buf[n] = '\0';
    int got = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < k && got < max; i++)
                fds[got++] = ((int *)CMSG_DATA(c))[i];
        }
    return got;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    /* socketpair echo. */
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    CHECK(write(sp[0], "hello", 5) == 5, "write to pair");
    char buf[64];
    CHECK(read(sp[1], buf, sizeof buf) == 5 && memcmp(buf, "hello", 5) == 0, "read from pair");
    /* Non blocking read on an empty socket. */
    CHECK(fcntl(sp[1], F_SETFL, O_NONBLOCK) == 0, "set nonblock");
    errno = 0;
    CHECK(read(sp[1], buf, sizeof buf) == -1 && errno == EAGAIN, "nonblocking read gives EAGAIN");
    CHECK(fcntl(sp[1], F_GETFL) & O_NONBLOCK, "F_GETFL shows O_NONBLOCK");
    CHECK(fcntl(sp[1], F_SETFL, 0) == 0, "clear nonblock");
    /* Descriptor passing: a memfd with contents and a pipe end. */
    int mfd = memfd_create("test", 0);
    CHECK(mfd >= 0 && ftruncate(mfd, 8192) == 0, "memfd and ftruncate");
    char *map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    CHECK(map != MAP_FAILED, "map memfd");
    strcpy(map + 4096, "shared page");
    int pfd[2];
    CHECK(pipe(pfd) == 0, "pipe");
    int pass[2] = { mfd, pfd[0] };
    CHECK(send_with_fds(sp[0], "fds", pass, 2) == 3, "sendmsg with two descriptors");
    CHECK(write(sp[0], "tail", 4) == 4, "data after the descriptors");
    int got[4];
    int n = recv_with_fds(sp[1], buf, 4, got, 4);       /* a stream: read the first message's 3 bytes */
    CHECK(n == 2 && strcmp(buf, "fds") == 0, "recvmsg delivers two descriptors with the message: %d '%s'", n, buf);
    CHECK(got[0] != mfd && got[1] != pfd[0], "received descriptors are new numbers");
    char *map2 = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, got[0], 0);
    CHECK(map2 != MAP_FAILED && strcmp(map2 + 4096, "shared page") == 0, "passed memfd shows the same page");
    CHECK(write(pfd[1], "p", 1) == 1 && read(got[1], buf, 1) == 1 && buf[0] == 'p', "passed pipe end reads the pipe");
    n = recv_with_fds(sp[1], buf, sizeof buf, got, 4);
    CHECK(n == 0 && strcmp(buf, "tail") == 0, "the next message carries no descriptors: '%s'", buf);
    /* Listener across fork. */
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { AF_UNIX, "socktest" };
    CHECK(ls >= 0 && bind(ls, (struct sockaddr *)&addr, sizeof addr) == 0, "bind");
    CHECK(bind(socket(AF_UNIX, SOCK_STREAM, 0), (struct sockaddr *)&addr, sizeof addr) == 0, "a second bind of the name before listen is allowed");
    CHECK(listen(ls, 4) == 0, "listen");
    struct pollfd pl = { ls, POLLIN, 0 };
    CHECK(poll(&pl, 1, 0) == 0, "no pending connection");
    pid_t pid = fork();
    if (pid == 0) {
        int cs = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(cs, (struct sockaddr *)&addr, sizeof addr) < 0)
            _exit(2);
        char reply[16];
        if (write(cs, "ping", 4) != 4 || read(cs, reply, sizeof reply) != 4 || memcmp(reply, "pong", 4) != 0)
            _exit(3);
        close(cs);
        _exit(0);
    }
    CHECK(poll(&pl, 1, 3000) == 1 && (pl.revents & POLLIN), "listener becomes readable");
    int as = accept(ls, NULL, NULL);
    CHECK(as >= 0, "accept");
    CHECK(read(as, buf, sizeof buf) == 4 && memcmp(buf, "ping", 4) == 0, "server reads ping");
    CHECK(write(as, "pong", 4) == 4, "server writes pong");
    int status;
    waitpid(pid, &status, 0);
    CHECK(status == 0, "client exit status 0x%x", status);
    /* Peer closed: POLLHUP, end of file, EPIPE. */
    struct pollfd ph = { as, POLLIN | POLLOUT, 0 };
    CHECK(poll(&ph, 1, 1000) == 1 && (ph.revents & POLLHUP), "POLLHUP after the peer closed: %x", ph.revents);
    CHECK(read(as, buf, sizeof buf) == 0, "end of file after the peer closed");
    errno = 0;
    CHECK(write(as, "x", 1) == -1 && errno == EPIPE, "EPIPE on write to a closed peer");
    int cs = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un none = { AF_UNIX, "nobody" };
    errno = 0;
    CHECK(connect(cs, (struct sockaddr *)&none, sizeof none) == -1 && errno == ECONNREFUSED, "connect to a missing name");
    /* Large transfer through the ring. */
    char *big = malloc(200000);
    for (int i = 0; i < 200000; i++)
        big[i] = (char)(i * 7);
    pid = fork();
    if (pid == 0) {
        long done = 0;
        while (done < 200000) {
            long k = write(sp[0], big + done, 200000 - done);
            if (k <= 0)
                _exit(4);
            done += k;
        }
        _exit(0);
    }
    long total = 0;
    int ok = 1;
    while (total < 200000) {
        long k = read(sp[1], buf, sizeof buf);
        if (k <= 0) {
            ok = 0;
            break;
        }
        for (long i = 0; i < k; i++)
            if (buf[i] != big[total + i])
                ok = 0;
        total += k;
    }
    waitpid(pid, &status, 0);
    CHECK(ok && total == 200000 && status == 0, "200000 bytes through the ring in order");
    printf("socktest: %d failures\n", failures);
    return failures ? 1 : 0;
}
