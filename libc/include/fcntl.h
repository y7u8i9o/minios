#pragma once
#include <minios/abi.h>

int open(const char *path, int flags, ...);
/* Relative paths are resolved from the directory dirfd, or from the working
 * directory when dirfd is AT_FDCWD. */
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, unsigned mode);
/* F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL, F_SETFL */
int fcntl(int fd, int cmd, ...);
