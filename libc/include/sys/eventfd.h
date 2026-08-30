#pragma once
#include <stdint.h>
#include <minios/abi.h>

int eventfd(unsigned initval, int flags);
int eventfd_read(int fd, uint64_t *value);
int eventfd_write(int fd, uint64_t value);
