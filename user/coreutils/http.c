/* http: a minimal HTTP/1.0 GET client (N11). Plain http:// only: there is
 * no TLS, no certificate validation and no https://. Redirects are not
 * followed; the status line and headers go to stderr with -v.
 *   http [-v] [-o FILE] http://HOST[:PORT]/PATH */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>

int main(int argc, char **argv)
{
    int verbose = 0, opt;
    const char *output = NULL;
    while ((opt = getopt(argc, argv, "vo:")) != -1) {
        if (opt == 'v')
            verbose = 1;
        else if (opt == 'o')
            output = optarg;
        else
            return fprintf(stderr, "usage: http [-v] [-o FILE] http://HOST[:PORT]/PATH\n"), 2;
    }
    if (optind != argc - 1)
        return fprintf(stderr, "usage: http [-v] [-o FILE] http://HOST[:PORT]/PATH\n"), 2;
    const char *url = argv[optind];
    if (strncmp(url, "http://", 7) != 0)
        return fprintf(stderr, "http: only http:// URLs are supported (no TLS)\n"), 2;
    char host[256], port[8] = "80";
    const char *p = url + 7;
    size_t n = strcspn(p, "/:");
    if (!n || n >= sizeof host)
        return fprintf(stderr, "http: bad host in %s\n", url), 2;
    memcpy(host, p, n);
    host[n] = 0;
    p += n;
    if (*p == ':') {
        size_t m = strcspn(++p, "/");
        if (!m || m >= sizeof port)
            return fprintf(stderr, "http: bad port in %s\n", url), 2;
        memcpy(port, p, m);
        port[m] = 0;
        p += m;
    }
    const char *path = *p ? p : "/";
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai;
    int error = getaddrinfo(host, port, &hints, &ai);
    if (error)
        return fprintf(stderr, "http: %s: %s\n", host, gai_strerror(error)), 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) < 0)
        return fprintf(stderr, "http: connect %s:%s: %s\n", host, port, strerror(errno)), 1;
    freeaddrinfo(ai);
    char request[1024];
    int len = snprintf(request, sizeof request,
                       "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: minios-http\r\nConnection: close\r\n\r\n",
                       path, host);
    if (len < 0 || len >= (int)sizeof request || write(fd, request, (size_t)len) != len)
        return fprintf(stderr, "http: request failed: %s\n", strerror(errno)), 1;
    shutdown(fd, SHUT_WR);
    FILE *out = output ? fopen(output, "w") : stdout;
    if (!out)
        return fprintf(stderr, "http: %s: %s\n", output, strerror(errno)), 1;
    char buf[8192];
    size_t have = 0;
    int in_headers = 1, status = 0;
    for (;;) {
        ssize_t r = read(fd, buf + have, sizeof buf - have);
        if (r < 0)
            return fprintf(stderr, "http: read: %s\n", strerror(errno)), 1;
        if (r == 0)
            break;
        have += (size_t)r;
        if (in_headers) {
            buf[have < sizeof buf ? have : sizeof buf - 1] = 0;
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) {
                if (have == sizeof buf)
                    return fprintf(stderr, "http: headers too long\n"), 1;
                continue;
            }
            *end = 0;
            if (sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1)
                return fprintf(stderr, "http: malformed status line\n"), 1;
            if (verbose)
                fprintf(stderr, "%s\n", buf);
            size_t body = (size_t)(end + 4 - buf);
            memmove(buf, buf + body, have - body);
            have -= body;
            in_headers = 0;
        }
        if (have && fwrite(buf, 1, have, out) != have)
            return fprintf(stderr, "http: write: %s\n", strerror(errno)), 1;
        have = 0;
    }
    if (in_headers)
        return fprintf(stderr, "http: connection closed before the response\n"), 1;
    if (out != stdout)
        fclose(out);
    close(fd);
    if (status < 200 || status >= 300)
        return fprintf(stderr, "http: server returned status %d\n", status), 1;
    return 0;
}
