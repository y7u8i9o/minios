#pragma once
#include <kernel.h>
#include <sync/spinlock.h>
#include <sched/wait.h>

/* Counting semaphore. lock protects count. */
struct semaphore {
    struct spinlock lock;
    int count;
    struct waitq wq;
};

void semaphore_init(struct semaphore *s, int count, const char *name);
void semaphore_down(struct semaphore *s);
bool semaphore_trydown(struct semaphore *s);
void semaphore_up(struct semaphore *s);
