#pragma once
#include <stddef.h>
#include <stdint.h>

/* The CRC-32 of gzip, zlib and PNG (IEEE 802.3, reflected, polynomial
 * 0xedb88320). crc32 continues the checksum crc over n more bytes. The
 * checksum of no bytes is 0, so a first call passes 0. The kernel has
 * the same function in kernel/lib/crc32.c. */
uint32_t crc32(uint32_t crc, const void *data, size_t n);
