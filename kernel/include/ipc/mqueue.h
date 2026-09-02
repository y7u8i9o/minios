#pragma once
#include <kernel.h>
#include <minios/abi.h>

struct file;

/* Named message queues and named shared memory objects (M17). */
int mq_open(const char *name, int flags, struct file **out);
int mq_unlink(const char *name);
int shm_open(const char *name, int flags, size_t size, struct file **out);
int shm_unlink(const char *name);

/* Wake every thread blocked in poll; descriptor producers call this after
 * changing readiness, and signal posting uses it to close poll's signal
 * check-to-sleep race. */
void poll_notify(void);
/* Block until one of the files is ready. timeout_ms < 0 blocks, 0 checks
 * once. Returns the number of ready entries. */
long poll_files(struct file **files, struct pollfd *pfds, size_t n, long timeout_ms);
/* M23: anonymous shared memory (memfd). */
int shm_create_anon(struct file **out);
