#pragma once
#include <kernel.h>
#include <sync/spinlock.h>
#include <sync/mutex.h>
#include <sched/wait.h>

/* Condition variable used with a mutex. lock orders waiters against
 * signals: condvar.lock -> mutex.lock. */
struct condvar {
    struct spinlock lock;
    struct waitq wq;
};

void condvar_init(struct condvar *cv, const char *name);
/* Release m, wait for a signal, re-acquire m. */
void condvar_wait(struct condvar *cv, struct mutex *m);
void condvar_signal(struct condvar *cv);
void condvar_broadcast(struct condvar *cv);
