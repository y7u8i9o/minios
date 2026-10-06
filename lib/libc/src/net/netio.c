/* Socket helpers with timeouts (netio.h). */
#include "netio.h"
#include <errno.h>
#include <poll.h>
#include <unistd.h>

int net_wait(int fd, short events, int timeout)
{
    struct pollfd p = { .fd = fd, .events = events };
    for (;;) {
        int r = poll(&p, 1, timeout > 0 ? timeout * 1000 : -1);
        if (r >= 0)
            return r > 0;
        if (errno != EINTR)
            return -errno;
    }
}

int net_send(int fd, const void *data, size_t len, int timeout)
{
    const char *p = data;
    while (len) {
        int ready = net_wait(fd, POLLOUT, timeout);
        if (ready <= 0)
            return ready ? ready : -ETIMEDOUT;
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -errno;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

ssize_t net_recv(int fd, void *buf, size_t len, int timeout)
{
    for (;;) {
        int ready = net_wait(fd, POLLIN, timeout);
        if (ready <= 0)
            return ready ? ready : -ETIMEDOUT;
        ssize_t n = read(fd, buf, len);
        if (n >= 0)
            return n;
        if (errno != EINTR && errno != EAGAIN)
            return -errno;
    }
}
