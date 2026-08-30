#pragma once
#include <sys/types.h>
#include <minios/abi.h>

/* Named message queues and shared memory objects, both as descriptors. */
int mq_open(const char *name, int flags);
int mq_unlink(const char *name);
ssize_t mq_send(int fd, const void *msg, size_t len);
ssize_t mq_recv(int fd, void *msg, size_t len);
int shm_open(const char *name, int flags, size_t size);
int shm_unlink(const char *name);
int poll(struct pollfd *fds, unsigned n, int timeout_ms);
