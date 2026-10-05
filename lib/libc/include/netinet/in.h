#pragma once
/* Internet addresses (N01): struct sockaddr_in, struct in_addr, the
 * INADDR_* constants and the IPPROTO_* numbers come from the shared ABI
 * header. Byte order conversion is in <arpa/inet.h> as well. */
#include <stdint.h>
#include <minios/abi.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

/* The size of the text of an IPv4 address with its NUL byte, for
 * inet_ntop. */
#define INET_ADDRSTRLEN 16

static inline uint16_t htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint16_t ntohs(uint16_t v)
{
    return htons(v);
}

static inline uint32_t htonl(uint32_t v)
{
    return (v << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
}

static inline uint32_t ntohl(uint32_t v)
{
    return htonl(v);
}
