#pragma once
#include <stddef.h>
#include <stdint.h>
/* Input has already been copied into kernel memory. These functions do not
 * dereference user addresses or substitute for user_range_ok. */
int socket_parse_address(const uint8_t *bytes, size_t length);
int socket_iovec_size(const size_t *lengths, size_t count, size_t *total);
