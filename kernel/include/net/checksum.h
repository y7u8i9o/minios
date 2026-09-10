#pragma once
/* The Internet checksum (RFC 1071): the one's complement of the one's
 * complement sum of the 16 bit big endian words of the data, an odd
 * trailing byte padded with a zero. A checksum is accumulated over
 * several pieces (pseudo header, header, payload) with
 * net_checksum_partial; every piece but the last must have an even
 * length, since the padding of an odd piece would misalign the next. The
 * result is in host order, ready to be stored with net_put_be16. */
#include <kernel.h>

uint32_t net_checksum_partial(const void *data, size_t len, uint32_t sum);
uint16_t net_checksum_finish(uint32_t sum);
uint16_t net_checksum(const void *data, size_t len);
