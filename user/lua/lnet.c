/* The net module: IPv4 TCP sockets as plain descriptors. A socket is an
 * integer descriptor, as in the descriptor functions of sys (lposix.c).
 * sys.close closes it, sys.poll and app:watch wait for it. Every socket
 * is close-on-exec. Failures return nil, the message and the errno, like
 * io.open. A timeout of -1 waits without limit, 0 does not wait, and a
 * positive timeout is in milliseconds; an expired timeout returns the
 * errno ETIMEDOUT. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "lauxlib.h"
#include "minios.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0          /* the host sets SO_NOSIGPIPE instead */
#endif

#define BACKLOG 16
#define RECV_MAX (1 << 20)

static int err_result(lua_State *L, int err)
{
    errno = err;
    return minios_errresult(L);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* wait_fd waits until fd reports events or the deadline passes. A
 * deadline below 0 waits without limit. Returns 0, or an errno value. */
static int wait_fd(int fd, short events, long long deadline)
{
    for (;;) {
        int timeout = -1;
        if (deadline >= 0) {
            long long left = deadline - now_ms();
            timeout = left > 0 ? (int)(left > 0x7fffffff ? 0x7fffffff : left) : 0;
        }
        struct pollfd p = { fd, events, 0 };
        int n = poll(&p, 1, timeout);
        if (n > 0)
            return 0;
        if (n == 0)
            return ETIMEDOUT;
        if (errno != EINTR)
            return errno;
    }
}

static long long deadline_of(lua_State *L, int index)
{
    lua_Integer ms = luaL_optinteger(L, index, -1);
    luaL_argcheck(L, ms >= -1, index, "timeout below -1");
    return ms < 0 ? -1 : now_ms() + ms;
}

static void set_cloexec(int fd)
{
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

static void set_nonblock(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}

static int new_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    set_cloexec(fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

static void push_address(lua_State *L, const struct sockaddr_in *a)
{
    char text[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &a->sin_addr, text, sizeof text))
        strcpy(text, "0.0.0.0");
    lua_pushstring(L, text);
    lua_pushinteger(L, ntohs(a->sin_port));
}

static int check_port(lua_State *L, int index)
{
    lua_Integer port = luaL_checkinteger(L, index);
    luaL_argcheck(L, port >= 0 && port <= 65535, index, "port outside 0 to 65535");
    return (int)port;
}

/* net.connect(host, port [, timeout_ms]) -> fd. The host is a name or a
 * numeric IPv4 address. The connection is blocking afterwards. */
static int net_connect(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    int port = check_port(L, 2);
    long long deadline = deadline_of(L, 3);
    struct addrinfo hints = { 0 }, *list = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, NULL, &hints, &list);
    if (gai != 0 || !list) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: %s", host, gai_strerror(gai));
        lua_pushinteger(L, ENOENT);
        return 3;
    }
    struct sockaddr_in addr;
    memcpy(&addr, list->ai_addr, sizeof addr);
    freeaddrinfo(list);
    addr.sin_port = htons((uint16_t)port);
    int fd = new_socket();
    if (fd < 0)
        return minios_errresult(L);
    set_nonblock(fd, 1);
    int err = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        err = errno;
        if (err == EINPROGRESS || err == EAGAIN || err == EINTR) {
            err = wait_fd(fd, POLLOUT, deadline);
            if (!err) {
                socklen_t len = sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
                    err = errno;
            }
        }
    }
    if (err) {
        close(fd);
        return err_result(L, err);
    }
    set_nonblock(fd, 0);
    lua_pushinteger(L, fd);
    return 1;
}

/* net.listen(port [, address = "0.0.0.0" [, backlog]]) -> fd, port. Port
 * 0 selects a free port, which the second result reports. */
static int net_listen(lua_State *L)
{
    int port = check_port(L, 1);
    const char *address = luaL_optstring(L, 2, "0.0.0.0");
    int backlog = (int)luaL_optinteger(L, 3, BACKLOG);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, address, &addr.sin_addr) != 1)
        return luaL_argerror(L, 2, "not a numeric IPv4 address");
    int fd = new_socket();
    if (fd < 0)
        return minios_errresult(L);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, backlog) < 0) {
        int err = errno;
        close(fd);
        return err_result(L, err);
    }
    socklen_t len = sizeof addr;
    getsockname(fd, (struct sockaddr *)&addr, &len);
    lua_pushinteger(L, fd);
    lua_pushinteger(L, ntohs(addr.sin_port));
    return 2;
}

/* net.accept(fd [, timeout_ms]) -> fd, address, port */
static int net_accept(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    long long deadline = deadline_of(L, 2);
    for (;;) {
        int err = wait_fd(fd, POLLIN, deadline);
        if (err)
            return err_result(L, err);
        struct sockaddr_in addr;
        socklen_t len = sizeof addr;
        int c = accept(fd, (struct sockaddr *)&addr, &len);
        if (c >= 0) {
            set_cloexec(c);
            set_nonblock(c, 0);
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            lua_pushinteger(L, c);
            push_address(L, &addr);
            return 3;
        }
        if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED)
            return minios_errresult(L);
    }
}

/* net.send(fd, data [, timeout_ms]) -> true. Sends all of data. The
 * timeout limits each wait for buffer space, not the whole call. */
static int net_send(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_Integer ms = luaL_optinteger(L, 3, -1);
    luaL_argcheck(L, ms >= -1, 3, "timeout below -1");
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, data + done, len - done, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR)
            return minios_errresult(L);
        int err = wait_fd(fd, POLLOUT, ms < 0 ? -1 : now_ms() + ms);
        if (err)
            return err_result(L, err);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* net.recv(fd, max [, timeout_ms]) -> data with 1 to max bytes, or nil
 * alone at the end of the stream. */
static int net_recv(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    lua_Integer max = luaL_checkinteger(L, 2);
    luaL_argcheck(L, max > 0 && max <= RECV_MAX, 2, "size outside 1 to 1048576");
    long long deadline = deadline_of(L, 3);
    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, (size_t)max);
    for (;;) {
        int err = wait_fd(fd, POLLIN, deadline);
        if (err)
            return err_result(L, err);
        ssize_t n = recv(fd, p, (size_t)max, MSG_DONTWAIT);
        if (n > 0) {
            luaL_pushresultsize(&b, (size_t)n);
            return 1;
        }
        if (n == 0) {
            lua_pushnil(L);
            return 1;
        }
        if (errno != EAGAIN && errno != EINTR)
            return minios_errresult(L);
    }
}

static const char *const shut_names[] = { "r", "w", "rw", NULL };

/* net.shutdown(fd [, "r" | "w" | "rw" = "w"]) -> true */
static int net_shutdown(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    static const int how[] = { SHUT_RD, SHUT_WR, SHUT_RDWR };
    if (shutdown(fd, how[luaL_checkoption(L, 2, "w", shut_names)]) < 0)
        return minios_errresult(L);
    lua_pushboolean(L, 1);
    return 1;
}

/* net.peer(fd) and net.address(fd) -> address, port of the other end and
 * of this end. */
static int name_of(lua_State *L, int peer)
{
    int fd = (int)luaL_checkinteger(L, 1);
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    int r = peer ? getpeername(fd, (struct sockaddr *)&addr, &len) : getsockname(fd, (struct sockaddr *)&addr, &len);
    if (r < 0)
        return minios_errresult(L);
    push_address(L, &addr);
    return 2;
}

static int net_peer(lua_State *L) { return name_of(L, 1); }
static int net_address(lua_State *L) { return name_of(L, 0); }

static const luaL_Reg net_funcs[] = {
    { "connect", net_connect },
    { "listen", net_listen },
    { "accept", net_accept },
    { "send", net_send },
    { "recv", net_recv },
    { "shutdown", net_shutdown },
    { "peer", net_peer },
    { "address", net_address },
    { NULL, NULL }
};

int luaopen_net(lua_State *L)
{
    luaL_newlib(L, net_funcs);
    return 1;
}
