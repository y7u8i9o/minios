#pragma once
/* Wire byte order (N02): the network is big endian, the CPU little
 * endian. Headers are read and written through the unaligned accessors
 * so that no wire structure is ever dereferenced at an odd address. */
#include <kernel.h>

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

static inline uint16_t net_get_be16(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)((b[0] << 8) | b[1]);
}

static inline uint32_t net_get_be32(const void *p)
{
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static inline void net_put_be16(void *p, uint16_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static inline void net_put_be32(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}
