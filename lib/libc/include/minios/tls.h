#pragma once
/* A TLS 1.3 client (RFC 8446) over a connected TCP socket
 * (docs/design/tls.md). The client offers X25519 and secp256r1, the
 * suites TLS_CHACHA20_POLY1305_SHA256 and TLS_AES_128_GCM_SHA256, and
 * verifies the certificate of the server against a trust store
 * (minios/x509.h). It sends no client certificate and does not resume
 * sessions. */
#include <stddef.h>
#include <sys/types.h>
#include <minios/x509.h>

/* The trust store of the system. The environment variable SSL_CERT_FILE
 * replaces the path in http_get. */
#define TLS_CA_FILE "/etc/ssl/cert.pem"

struct tls;

/* Runs the handshake on the connected socket fd for host, which the
 * certificate must match. The timeout bounds, in seconds, every wait for
 * the socket, and 0 sets no bound. Returns 0 and the connection in *out,
 * or a negative errno: -EPROTO when the handshake fails (an alert, a
 * protocol error or a certificate that does not verify), -ETIMEDOUT,
 * -ENOMEM, or the error of a read or a write. err receives the reason.
 * The socket remains open after a failure. */
int tls_connect(struct tls **out, int fd, const char *host, const struct x509_store *store, int timeout, char *err,
                size_t err_len);

/* Reads application data. Returns the number of bytes, 0 when the server
 * has closed the connection, or a negative errno as tls_connect. */
ssize_t tls_read(struct tls *t, void *buf, size_t len);

/* Writes all len bytes. Returns len or a negative errno. */
ssize_t tls_write(struct tls *t, const void *buf, size_t len);

/* The reason of the last failure of the connection. */
const char *tls_error(const struct tls *t);

/* Sends close_notify when the connection works, and frees the
 * connection. The socket remains open. */
void tls_close(struct tls *t);
