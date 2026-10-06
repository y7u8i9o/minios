/* This file is the HTTP/1.0 GET client of http(1) and pkg(1). The socket
 * is non blocking so that every wait, for the connection and for each
 * piece of the response, goes through poll with the caller's timeout. The
 * body is checked against Content-Length when the server sends one. A
 * connection closed early and surplus bytes are both errors, so a caller
 * never mistakes a truncated file for a complete one. An https:// URL
 * runs the request and the response through the TLS client of
 * minios/tls.h. */
#include <minios/http.h>
#include <minios/tls.h>
#include "netio.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>

#define HEAD_MAX 8192

int http_parse_url(const char *url, struct http_url *u)
{
    memset(u, 0, sizeof *u);
    const char *p;
    if (strncmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        p = url + 8;
        u->tls = 1;
    } else {
        return -EPROTONOSUPPORT;
    }
    size_t n = strcspn(p, "/:");
    if (!n || n >= sizeof u->host)
        return -EINVAL;
    memcpy(u->host, p, n);
    p += n;
    strcpy(u->port, u->tls ? "443" : "80");
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

/* A connection to the server: the socket, and the TLS state of an
 * https:// URL. */
struct conn {
    int s;
    struct tls *tls;
    int timeout;
};

/* conn_read returns the number of bytes, 0 at the end of the response, or
 * a negative errno. */
static ssize_t conn_read(struct conn *c, void *buf, size_t len)
{
    if (c->tls)
        return tls_read(c->tls, buf, len);
    return net_recv(c->s, buf, len, c->timeout);
}

static int conn_send(struct conn *c, const void *data, size_t len)
{
    if (c->tls) {
        ssize_t w = tls_write(c->tls, data, len);
        return w < 0 ? (int)w : 0;
    }
    return net_send(c->s, data, len, c->timeout);
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

/* receive reads the response from the connection. */
static int receive(struct conn *c, int fd, long long max_body, const char *where, struct http_response *res)
{
    char *buf = malloc(HEAD_MAX);
    if (!buf)
        return fail(res, -ENOMEM, "out of memory");
    size_t have = 0;
    int in_head = 1, r = 0;
    for (;;) {
        ssize_t n = conn_read(c, buf + have, HEAD_MAX - 1 - have);
        if (n == -ETIMEDOUT) {
            r = fail(res, -ETIMEDOUT, "%s sent nothing for %d seconds", where, c->timeout);
            break;
        }
        if (n < 0 && c->tls) {
            r = fail(res, (int)n, "TLS with %s: %s", where, tls_error(c->tls));
            break;
        }
        if (n < 0) {
            r = fail(res, (int)n, "read from %s: %s", where, strerror((int)-n));
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
        return fail(res, r, "only http:// and https:// URLs are supported");
    if (r < 0)
        return fail(res, r, "malformed URL %s", url);
    char where[HTTP_HOST_MAX + HTTP_PORT_MAX + 1];
    snprintf(where, sizeof where, "%s:%s", u.host, u.port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
    int gai = getaddrinfo(u.host, u.port, &hints, &ai);
    if (gai)
        return fail(res, -EHOSTUNREACH, "%s: %s", u.host, gai_strerror(gai));
    /* The host build of pkg (docs/design/packages.md) runs on systems
     * without SOCK_NONBLOCK, where fcntl makes the socket non blocking. */
#ifdef SOCK_NONBLOCK
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0 && fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) < 0) {
        close(s);
        s = -1;
    }
#endif
    if (s < 0) {
        freeaddrinfo(ai);
        return fail(res, -errno, "socket: %s", strerror(errno));
    }
    r = connect(s, ai->ai_addr, ai->ai_addrlen) < 0 ? -errno : 0;
    freeaddrinfo(ai);
    if (r == -EINPROGRESS) {
        int ready = net_wait(s, POLLOUT, timeout);
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
    struct conn c = { s, NULL, timeout };
    if (r == 0 && u.tls) {
        /* The trust store is needed only for the handshake. */
        const char *ca = getenv("SSL_CERT_FILE");
        if (!ca || !*ca)
            ca = TLS_CA_FILE;
        struct x509_store store = { 0 };
        int n = x509_store_load(&store, ca, NULL);
        if (n < 0) {
            r = fail(res, n, "trust store %s: %s", ca, strerror(-n));
        } else {
            char err[256];
            r = tls_connect(&c.tls, s, u.host, &store, timeout, err, sizeof err);
            if (r < 0)
                fail(res, r, "TLS with %s: %s", where, err);
        }
        x509_store_free(&store);
    }
    if (r == 0) {
        char request[HTTP_PATH_MAX + HTTP_HOST_MAX + 128];
        int len = snprintf(request, sizeof request,
                           "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: minios-http\r\nConnection: close\r\n\r\n",
                           u.path, u.host);
        r = conn_send(&c, request, (size_t)len);
        if (r < 0)
            fail(res, r, "sending the request to %s: %s", where,
                 c.tls && r == -EPROTO ? tls_error(c.tls) : strerror(-r));
    }
    if (r == 0) {
        /* TLS has no half close for the server to see, so only a plain
         * connection ends its direction early. */
        if (!c.tls)
            shutdown(s, SHUT_WR);
        r = receive(&c, fd, max_body, where, res);
    }
    tls_close(c.tls);
    close(s);
    return r;
}
