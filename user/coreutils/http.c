/* http is a minimal HTTP/1.0 GET client (N11). It speaks plain http://
 * only, without TLS, certificate validation or https://. Redirects are
 * not followed, and -v prints the status line and headers to stderr. The
 * protocol code is the libc client of minios/http.h, which pkg(1) shares.
 *   http [-v] [-t SECONDS] [-o FILE] http://HOST[:PORT]/PATH */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <minios/http.h>

static int usage(void)
{
    fprintf(stderr, "usage: http [-v] [-t SECONDS] [-o FILE] http://HOST[:PORT]/PATH\n");
    return 2;
}

int main(int argc, char **argv)
{
    int verbose = 0, timeout = 30, opt;
    const char *output = NULL;
    while ((opt = getopt(argc, argv, "vt:o:")) != -1) {
        if (opt == 'v')
            verbose = 1;
        else if (opt == 't')
            timeout = atoi(optarg);
        else if (opt == 'o')
            output = optarg;
        else
            return usage();
    }
    if (optind != argc - 1 || timeout < 0)
        return usage();
    const char *url = argv[optind];
    struct http_url parsed;
    int r = http_parse_url(url, &parsed);
    if (r == -EPROTONOSUPPORT)
        return fprintf(stderr, "http: only http:// URLs are supported (no TLS)\n"), 2;
    if (r < 0)
        return fprintf(stderr, "http: malformed URL %s\n", url), 2;
    int fd = 1;
    if (output && (fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
        return fprintf(stderr, "http: %s: %s\n", output, strerror(errno)), 1;
    struct http_response res;
    r = http_get(url, fd, timeout, 0, &res);
    if (verbose && res.status)
        fprintf(stderr, "%s\n", res.head);
    if (output)
        close(fd);
    if (r < 0) {
        /* A partial body is not left behind as if it were the file. */
        if (output)
            unlink(output);
        return fprintf(stderr, "http: %s\n", res.error), 1;
    }
    if (res.status < 200 || res.status >= 300)
        return fprintf(stderr, "http: server returned status %d\n", res.status), 1;
    return 0;
}
