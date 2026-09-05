#pragma once
#include <kernel.h>
#include <minios/abi.h>

struct file;

/* Named message queues and named shared memory objects (M17). */
int mq_open(const char *name, int flags, struct file **out);
int mq_unlink(const char *name);
int shm_open(const char *name, int flags, size_t size, struct file **out);
int shm_unlink(const char *name);

/* M23: anonymous shared memory (memfd). */
int shm_create_anon(struct file **out);
