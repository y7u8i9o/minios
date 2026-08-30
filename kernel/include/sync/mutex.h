#pragma once
#include <kernel.h>
#include <sync/spinlock.h>
#include <sched/wait.h>

struct thread;

/* Sleeping lock. lock protects locked and owner. Ordering:
 * mutex.lock -> waitq.lock -> sched_lock. */
struct mutex {
    struct spinlock lock;
    bool locked;
    struct thread *owner;
    struct waitq wq;
};

void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_held(struct mutex *m);
