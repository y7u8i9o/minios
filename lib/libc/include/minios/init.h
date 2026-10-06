#pragma once
/* Requests to init on its control socket, the abstract socket "init"
 * (docs/design/init.md). A request is one line, such as "poweroff",
 * "reboot" or "session 1000". The reply begins with "ok" or with
 * "error: " and a message, followed by the output of the request. */
#include <stddef.h>

/* Sends request, a line without its newline, and copies the reply,
 * terminated by a NUL, to reply (size bytes) when reply is not NULL.
 * Without reply the function does not wait for one. Returns the length of
 * the reply, 0 without reply, or -1 with errno when init cannot be
 * reached or the request is longer than 255 bytes. */
long init_request(const char *request, char *reply, size_t size);
