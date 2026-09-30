#pragma once
/* This header declares one HTTP/1.0 GET over plain http://
 * (docs/design/network.md), which http(1) and pkg(1) share. There is no
 * TLS and no https://, redirects are not followed and a chunked body is
 * not decoded. */
#include <stddef.h>

#define HTTP_HOST_MAX 256
#define HTTP_PORT_MAX 8
#define HTTP_PATH_MAX 1024

/* The port is "80" and the path "/" when the URL names neither. */
struct http_url {
    char host[HTTP_HOST_MAX];
    char port[HTTP_PORT_MAX];
    char path[HTTP_PATH_MAX];
};

/* http_parse_url splits http://HOST[:PORT][/PATH]. It returns 0,
 * -EPROTONOSUPPORT for another scheme, or -EINVAL for an empty or
 * oversized host, port or path. */
int http_parse_url(const char *url, struct http_url *u);

/* status holds the status code and stays 0 until the status line has
 * arrived. length holds Content-Length and is -1 when the server sent
 * none. received counts the body bytes written to the descriptor. head
 * holds the status line and the headers, truncated to fit, and error
 * holds the reason when http_get fails. */
struct http_response {
    int status;
    long long length;
    long long received;
    char head[2048];
    char error[256];
};

/* http_get sends GET for url and writes the response body to fd. The
 * timeout bounds, in seconds, the connection and every wait for data, and
 * 0 sets no bound. max_body refuses a body larger than that many bytes,
 * and 0 sets no bound. http_get returns 0 when a complete response has
 * arrived, whatever its status, which res->status holds. Otherwise it
 * returns a negative errno and describes the failure in res->error. The
 * errors are -EPROTONOSUPPORT or -EINVAL for the URL, -EHOSTUNREACH when
 * the name does not resolve, the error of connect, -ETIMEDOUT, -EIO for a
 * malformed response or a body that is shorter or longer than its
 * Content-Length, -EFBIG for a body above max_body, and the error of the
 * write to fd. */
int http_get(const char *url, int fd, int timeout, long long max_body, struct http_response *res);
