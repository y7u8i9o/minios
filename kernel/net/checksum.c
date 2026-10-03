/* RFC 1071 checksum over unaligned data. The sum is retained in 32 bits and
 * folded at the end; a piece of at most 64 KiB cannot overflow it. */
#include <net/checksum.h>
#include <net/byteorder.h>

uint32_t net_checksum_partial(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;
    while (len >= 2) {
        sum += net_get_be16(p);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8;
    /* Fold so that a long run of pieces cannot overflow. */
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return sum;
}

uint16_t net_checksum_finish(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t net_checksum(const void *data, size_t len)
{
    return net_checksum_finish(net_checksum_partial(data, len, 0));
}
