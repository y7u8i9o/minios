#pragma once
#include <sys/types.h>
#include <minios/abi.h>

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
/* Anonymous shared memory descriptor, sized with ftruncate (M23). */
int memfd_create(const char *name, unsigned flags);
