#pragma once
#include <stddef.h>
#include <stdint.h>

/* The CRC-32 of the gzip format over n bytes. */
uint32_t gzip_crc32(const uint8_t *p, size_t n);
/* Compress or decompress a whole buffer; *dst is allocated with malloc.
 * Both return 0, or -1 for invalid data or when memory runs out. */
int gzip_compress(const uint8_t *src, size_t len, uint8_t **dst, size_t *dstlen);
int gzip_decompress(const uint8_t *src, size_t len, uint8_t **dst, size_t *dstlen);
