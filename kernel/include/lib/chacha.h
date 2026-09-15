#pragma once
#include <stdint.h>
/* RFC 8439 block function. Input and output are host-order 32-bit words. */
void chacha20_block(const uint32_t input[16], uint32_t output[16]);
