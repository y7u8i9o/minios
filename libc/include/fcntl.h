#pragma once
#include <minios/abi.h>

int open(const char *path, int flags, ...);
int creat(const char *path, unsigned mode);
/* F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL, F_SETFL */
int fcntl(int fd, int cmd, ...);
