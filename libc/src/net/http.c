/* This file is the HTTP/1.0 GET client of http(1) and pkg(1). The socket
 * is non blocking so that every wait, for the connection and for each
 * piece of the response, goes through poll with the caller's timeout. The
 * body is checked against Content-Length when the server sends one. A
 * connection closed early and surplus bytes are both errors, so a caller
 * never mistakes a truncated file for a complete one. */
#include <minios/http.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/socket.h>

#define HEAD_MAX 8192

int http_parse_url(const char *url, struct http_url *u)
{
    memset(u, 0, sizeof *u);
    if (strncmp(url, "http://", 7) != 0)
        return -EPROTONOSUPPORT;
    const char *p = url + 7;
    size_t n = strcspn(p, "/:");
    if (!n || n >= sizeof u->host)
        return -EINVAL;
    memcpy(u->host, p, n);
    p += n;
    strcpy(u->port, "80");
    if (*p == ':') {
        size_t m = strcspn(++p, "/");
        if (!m || m >= sizeof u->port)
            return -EINVAL;
        memcpy(u->port, p, m);
        u->port[m] = '\0';
        for (size_t i = 0; i < m; i++)
            if (u->port[i] < '0' || u->port[i] > '9')
                return -EINVAL;
        p += m;
    }
    const char *path = *p ? p : "/";
    if (strlen(path) >= sizeof u->path)
        return -EINVAL;
    strcpy(u->path, path);
    return 0;
}

static int fail(struct http_response *res, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(struct http_response *res, int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res->error, sizeof res->error, fmt, ap);
    va_end(ap);
    return err;
}

/* wait_for waits for events on fd. It returns 1 when fd is ready, 0
 * after timeout seconds, and a negative errno when poll fails. */
static int wait_for(int fd, short events, int timeout)
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

static int write_all(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t w = write(fd, data, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        data += w;
        len -= (size_t)w;
    }
    return 0;
}

/* parse_head reads the status line and the Content-Length header of a
 * complete head. */
static int parse_head(char *head, struct http_response *res)
{
    if (sscanf(head, "HTTP/%*d.%*d %d", &res->status) != 1 || res->status < 100 || res->status > 999)
        return fail(res, -EIO, "malformed status line");
    for (char *line = strstr(head, "\r\n"); line; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, "Content-Length:", 15) != 0)
            continue;
        char *end;
        const char *v = line + 15;
        while (*v == ' ' || *v == '\t')
            v++;
        long long n = strtoll(v, &end, 10);
        if (end == v || n < 0 || (*end && *end != '\r' && *end != ' ' && *end != '\t'))
            return fail(res, -EIO, "malformed Content-Length");
        res->length = n;
    }
    return 0;
}

/* receive reads the response from the connected socket s. */
static int receive(int s, int fd, int timeout, long long max_body, const char *where, struct http_response *res)
{
    char *buf = malloc(HEAD_MAX);
    if (!buf)
        return fail(res, -ENOMEM, "out of memory");
    size_t have = 0;
    int in_head = 1, r = 0;
    for (;;) {
        int ready = wait_for(s, POLLIN, timeout);
        if (ready < 0) {
            r = fail(res, ready, "poll: %s", strerror(-ready));
            break;
        }
        if (ready == 0) {
            r = fail(res, -ETIMEDOUT, "%s sent nothing for %d seconds", where, timeout);
            break;
        }
        ssize_t n = read(s, buf + have, HEAD_MAX - 1 - have);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            r = fail(res, -errno, "read from %s: %s", where, strerror(errno));
            break;
        }
        if (n == 0) {
            if (in_head)
                r = fail(res, -EIO, "%s closed the connection before the response", where);
            else if (res->length >= 0 && res->received < res->length)
                r = fail(res, -EIO, "%s closed the connection after %lld of %lld bytes", where,
                         res->received, res->length);
            break;
        }
        have += (size_t)n;
        if (in_head) {
            buf[have] = '\0';
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) {
                if (have == HEAD_MAX - 1) {
                    r = fail(res, -EIO, "the response headers are longer than %d bytes", HEAD_MAX);
                    break;
                }
                continue;
            }
            *end = '\0';
            strncpy(res->head, buf, sizeof res->head - 1);
            if ((r = parse_head(buf, res)) < 0)
                break;
            if (max_body > 0 && res->length > max_body) {
                r = fail(res, -EFBIG, "%s announces %lld bytes, more than the %lld expected", where,
                         res->length, max_body);
                break;
            }
            size_t body = (size_t)(end + 4 - buf);
            memmove(buf, buf + body, have - body);
            have -= body;
            in_head = 0;
        }
        if (res->length >= 0 && res->received + (long long)have > res->length) {
            r = fail(res, -EIO, "%s sent more than its Content-Length of %lld bytes", where, res->length);
            break;
        }
        if (max_body > 0 && res->received + (long long)have > max_body) {
            r = fail(res, -EFBIG, "%s sent more than the %lld bytes expected", where, max_body);
            break;
        }
        if (have && (r = write_all(fd, buf, have)) < 0) {
            fail(res, r, "write: %s", strerror(-r));
            break;
        }
        res->received += (long long)have;
        have = 0;
    }
    free(buf);
    return r;
}

int http_get(const char *url, int fd, int timeout, long long max_body, struct http_response *res)
{
    memset(res, 0, sizeof *res);
    res->length = -1;
    struct http_url u;
    int r = http_parse_url(url, &u);
    if (r == -EPROTONOSUPPORT)
        return fail(res, r, "only http:// URLs are supported (there is no TLS)");
    if (r < 0)
        return fail(res, r, "malformed URL %s", url);
    char where[HTTP_HOST_MAX + HTTP_PORT_MAX + 1];
    snprintf(where, sizeof where, "%s:%s", u.host, u.port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
    int gai = getaddrinfo(u.host, u.port, &hints, &ai);
    if (gai)
        return fail(res, -EHOSTUNREACH, "%s: %s", u.host, gai_strerror(gai));
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) {
        freeaddrinfo(ai);
        return fail(res, -errno, "socket: %s", strerror(errno));
    }
    r = connect(s, ai->ai_addr, ai->ai_addrlen) < 0 ? -errno : 0;
    freeaddrinfo(ai);
    if (r == -EINPROGRESS) {
        int ready = wait_for(s, POLLOUT, timeout);
        if (ready == 0) {
            r = fail(res, -ETIMEDOUT, "connect to %s: no answer in %d seconds", where, timeout);
        } else if (ready < 0) {
            r = fail(res, ready, "poll: %s", strerror(-ready));
        } else {
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
                err = errno;
            r = err ? fail(res, -err, "connect to %s: %s", where, strerror(err)) : 0;
        }
    } else if (r < 0) {
        fail(res, r, "connect to %s: %s", where, strerror(-r));
    }
    if (r == 0) {
        char request[HTTP_PATH_MAX + HTTP_HOST_MAX + 128];
        int len = snprintf(request, sizeof request,
                           "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: minios-http\r\nConnection: close\r\n\r\n",
                           u.path, u.host);
        for (int done = 0; r == 0 && done < len; ) {
            int ready = wait_for(s, POLLOUT, timeout);
            if (ready <= 0) {
                r = fail(res, ready ? ready : -ETIMEDOUT, "sending the request to %s: %s", where,
                         ready ? strerror(-ready) : "timed out");
                break;
            }
            ssize_t w = write(s, request + done, (size_t)(len - done));
            if (w < 0 && errno != EINTR && errno != EAGAIN)
                r = fail(res, -errno, "sending the request to %s: %s", where, strerror(errno));
            else if (w > 0)
                done += (int)w;
        }
    }
    if (r == 0) {
        shutdown(s, SHUT_WR);
        r = receive(s, fd, timeout, max_body, where, res);
    }
    close(s);
    return r;
}
