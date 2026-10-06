#pragma once
/* Socket helpers of the HTTP client (http.c) and the TLS client (tls.c).
 * The socket may be non blocking. Every wait goes through poll with a
 * timeout in seconds, and 0 sets no bound. */
#include <stddef.h>
#include <sys/types.h>

/* Waits for events on fd. Returns 1 when fd is ready, 0 after timeout
 * seconds, and a negative errno when poll fails. */
int net_wait(int fd, short events, int timeout);
/* Sends len bytes. Returns 0, -ETIMEDOUT, or the negative errno of
 * write or poll. */
int net_send(int fd, const void *data, size_t len, int timeout);
/* Receives up to len bytes after a wait. Returns the number of bytes, 0
 * at the end of the stream, -ETIMEDOUT, or the negative errno of read or
 * poll. */
ssize_t net_recv(int fd, void *buf, size_t len, int timeout);
