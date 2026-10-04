#pragma once
/* Little endian values at any alignment, read byte by byte: fields of
 * firmware tables and device descriptors. The network code reads big
 * endian wire fields with <net/byteorder.h>. */
#include <kernel.h>

static inline uint16_t get_le16(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)(b[0] | b[1] << 8);
}

static inline uint32_t get_le32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static inline uint64_t get_le64(const void *p)
{
    return (uint64_t)get_le32(p) | (uint64_t)get_le32((const uint8_t *)p + 4) << 32;
}
