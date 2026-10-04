#pragma once
#include <sys/types.h>
#include <minios/abi.h>

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
/* Change the protection of whole pages (M37). */
int mprotect(void *addr, size_t len, int prot);
/* Write the dirty pages of a shared file mapping back (M37). */
int msync(void *addr, size_t len, int flags);
/* Advice about the use of a range (M38). */
int madvise(void *addr, size_t len, int advice);
/* Anonymous shared memory descriptor, sized with ftruncate (M23). */
int memfd_create(const char *name, unsigned flags);
