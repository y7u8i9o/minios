/* Requests to init (minios/init.h). */
#include <minios/init.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

long init_request(const char *request, char *reply, size_t size)
{
    char line[257];
    size_t len = strlen(request);
    if (len > 255) {
        errno = E2BIG;
        return -1;
    }
    memcpy(line, request, len);
    line[len++] = '\n';
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr = { AF_UNIX, "init" };
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || write(fd, line, len) != (ssize_t)len) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    size_t got = 0;
    if (reply && size > 0) {
        while (got < size - 1) {
            ssize_t n = read(fd, reply + got, size - 1 - got);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        reply[got] = '\0';
    }
    close(fd);
    return (long)got;
}
