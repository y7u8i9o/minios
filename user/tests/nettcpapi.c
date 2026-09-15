/* N06 socket ABI: connection completion, half-close and file lifetime. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int failures;
static volatile sig_atomic_t interrupted;
#define CHECK(condition, description)                                                              \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            printf("nettcpapi: FAIL %s (errno %d)\n", description, errno);                         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static struct sockaddr_in address(unsigned port)
{
    struct sockaddr_in name = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port),
    };
    return name;
}
static int ready(int fd, short events)
{
    struct pollfd descriptor = {
        .fd = fd,
        .events = events,
    };
    return poll(&descriptor, 1, 3000) == 1 && (descriptor.revents & events);
}
static int listener(unsigned port, int flags)
{
    int fd = socket(AF_INET, SOCK_STREAM | flags, 0);
    struct sockaddr_in name = address(port);
    CHECK(fd >= 0, "create listener");
    CHECK(bind(fd, (struct sockaddr *)&name, sizeof name) == 0, "bind listener");
    CHECK(listen(fd, 4) == 0, "listen");
    return fd;
}

static void nonblocking_checks(void)
{
    int server = listener(8800, SOCK_NONBLOCK);
    CHECK(accept(server, NULL, NULL) == -1 && errno == EAGAIN, "empty nonblocking accept");
    int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    CHECK(client >= 0 && (fcntl(client, F_GETFD) & FD_CLOEXEC), "TCP type flags");
    struct sockaddr_in name = address(8800);
    CHECK(connect(client, (struct sockaddr *)&name, sizeof name) == -1 && errno == EINPROGRESS,
          "nonblocking connect returns EINPROGRESS");
    CHECK(ready(client, POLLOUT), "poll completes connect");
    int error = -1;
    socklen_t length = sizeof error;
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error,
          "SO_ERROR confirms connect success");
    CHECK(ready(server, POLLIN), "listener becomes readable");
    struct sockaddr_in peer;
    memset(&peer, 0x55, sizeof peer);
    length = 2;
    int accepted = accept4(server, (struct sockaddr *)&peer, &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
    CHECK(accepted >= 0 && length == sizeof peer && peer.sin_family == AF_INET,
          "accept reports full peer length to short buffer");
    CHECK((fcntl(accepted, F_GETFL) & O_NONBLOCK) && (fcntl(accepted, F_GETFD) & FD_CLOEXEC),
          "accepted file flags");
    length = sizeof peer;
    CHECK(getpeername(client, (struct sockaddr *)&peer, &length) == 0 &&
              peer.sin_port == name.sin_port && peer.sin_addr.s_addr == name.sin_addr.s_addr,
          "connected peer name");
    close(server);

    int copy = dup(client);
    CHECK(copy >= 0, "duplicate connection");
    close(client);
    CHECK(write(copy, "tail", 4) == 4, "duplicate survives original close and writes");
    CHECK(shutdown(copy, SHUT_WR) == 0, "shutdown follows queued data");
    CHECK(send(copy, "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE,
          "write after local half-close fails without SIGPIPE");
    CHECK(ready(accepted, POLLIN), "data or EOF wakes receiver");
    char buffer[16];
    CHECK(recv(accepted, buffer, 2, MSG_PEEK) == 2 && !memcmp(buffer, "ta", 2), "peek stream");
    CHECK(read(accepted, buffer, sizeof buffer) == 4 && !memcmp(buffer, "tail", 4),
          "read copies queued stream data");
    CHECK(ready(accepted, POLLIN) && recv(accepted, buffer, sizeof buffer, 0) == 0,
          "EOF arrives after queued data");
    CHECK(write(accepted, "reply", 5) == 5, "peer half-close retains write direction");
    CHECK(shutdown(accepted, SHUT_WR) == 0, "close remaining direction");
    CHECK(ready(copy, POLLIN) && recv(copy, buffer, sizeof buffer, 0) == 5 &&
              !memcmp(buffer, "reply", 5),
          "half-closed endpoint receives reply");
    CHECK(ready(copy, POLLIN) && read(copy, buffer, sizeof buffer) == 0, "final EOF");
    close(copy);
    close(accepted);

    client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    name = address(8899);
    CHECK(connect(client, (struct sockaddr *)&name, sizeof name) == -1 && errno == EINPROGRESS,
          "refused connection starts asynchronously");
    CHECK(ready(client, POLLOUT), "refused connect wakes poll");
    length = sizeof error;
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == ECONNREFUSED,
          "poll failure reported by SO_ERROR");
    length = sizeof error;
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error,
          "SO_ERROR consumed once");
    close(client);
}

struct blocking_client {
    unsigned port;
    int result;
};
static void *client_thread(void *argument)
{
    struct blocking_client *client = argument;
    client->result = -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in name = address(client->port);
    usleep(20000);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&name, sizeof name) == 0 &&
        write(fd, "q", 1) == 1 && shutdown(fd, SHUT_WR) == 0) {
        char byte;
        if (read(fd, &byte, 1) == 1 && byte == 'r' && read(fd, &byte, 1) == 0)
            client->result = 0;
    }
    close(fd);
    return NULL;
}
static void blocking_checks(void)
{
    int server = listener(8801, 0);
    struct blocking_client client = {
        .port = 8801,
        .result = -1,
    };
    pthread_t thread;
    int created = pthread_create(&thread, NULL, client_thread, &client);
    CHECK(created == 0, "start blocking client");
    if (!created) {
        int accepted = accept(server, NULL, NULL);
        CHECK(accepted >= 0, "blocking accept wakes on completed handshake");
        char byte;
        CHECK(read(accepted, &byte, 1) == 1 && byte == 'q', "blocking receive wakes on data");
        CHECK(read(accepted, &byte, 1) == 0, "blocking receive wakes on FIN");
        CHECK(write(accepted, "r", 1) == 1 && shutdown(accepted, SHUT_WR) == 0,
              "blocking response");
        close(accepted);
        pthread_join(thread, NULL);
        CHECK(client.result == 0, "blocking connect and full-duplex completion");
    }
    close(server);
}
static void on_signal(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}
static void lifetime_checks(void)
{
    int server = listener(8802, 0);
    signal(SIGUSR1, on_signal);
    pid_t parent = getpid();
    pid_t child = fork();
    CHECK(child >= 0, "fork signal sender");
    if (!child) {
        usleep(50000);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    if (child > 0) {
        CHECK(accept(server, NULL, NULL) == -1 && errno == EINTR && interrupted,
              "signal interrupts blocking accept");
        waitpid(child, NULL, 0);
    }
    child = fork();
    CHECK(child >= 0, "fork connecting process");
    if (!child) {
        close(server);
        int client = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in name = address(8802);
        int ok = client >= 0 && connect(client, (struct sockaddr *)&name, sizeof name) == 0 &&
                 write(client, "exit", 4) == 4;
        /* Process exit releases the final file while data awaits its ACK. */
        _exit(ok ? 0 : 1);
    }
    if (child > 0) {
        int accepted = accept(server, NULL, NULL);
        char buffer[8];
        CHECK(accepted >= 0 && read(accepted, buffer, sizeof buffer) == 4 &&
                  !memcmp(buffer, "exit", 4),
              "process-exit close retains queued data");
        CHECK(read(accepted, buffer, sizeof buffer) == 0, "process-exit close sends FIN");
        close(accepted);
        int status = -1;
        waitpid(child, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "connecting child exited cleanly");
    }
    close(server);
}
int main(void)
{
    nonblocking_checks();
    blocking_checks();
    lifetime_checks();
    printf("nettcpapi: %d failures\n", failures);
    return failures ? 1 : 0;
}
