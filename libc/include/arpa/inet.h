#pragma once
/* Byte order conversion (N01) and IPv4 address text conversion (N11).
 * Only AF_INET is supported; inet_pton and inet_ntop fail with
 * EAFNOSUPPORT for every other family. */
#include <netinet/in.h>

int inet_aton(const char *text, struct in_addr *out);
in_addr_t inet_addr(const char *text);
char *inet_ntoa(struct in_addr in);
int inet_pton(int family, const char *text, void *out);
const char *inet_ntop(int family, const void *in, char *out, socklen_t size);
