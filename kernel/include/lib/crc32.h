#pragma once
#include <kernel.h>

/* CRC-32 (IEEE 802.3, reflected, initial 0xffffffff, final xor). */
uint32_t crc32(uint32_t crc, const void *data, size_t n);
