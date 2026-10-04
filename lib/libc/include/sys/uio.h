#pragma once
#include <sys/types.h>
#include <minios/abi.h>   /* struct iovec */

#define IOV_MAX 1024

ssize_t readv(int fd, const struct iovec *iov, int count);
ssize_t writev(int fd, const struct iovec *iov, int count);
