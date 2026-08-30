#include <sync/condvar.h>

void condvar_init(struct condvar *cv, const char *name)
{
    spinlock_init(&cv->lock, name);
    waitq_init(&cv->wq, name);
}

void condvar_wait(struct condvar *cv, struct mutex *m)
{
    /* cv->lock is held from before the mutex is released until the thread
     * is on the wait queue, so a signal in between cannot be missed. */
    spin_lock(&cv->lock);
    mutex_unlock(m);
    waitq_wait(&cv->wq, &cv->lock);
    spin_unlock(&cv->lock);
    mutex_lock(m);
}

void condvar_signal(struct condvar *cv)
{
    spin_lock(&cv->lock);
    waitq_wake_one(&cv->wq);
    spin_unlock(&cv->lock);
}

void condvar_broadcast(struct condvar *cv)
{
    spin_lock(&cv->lock);
    waitq_wake_all(&cv->wq);
    spin_unlock(&cv->lock);
}
