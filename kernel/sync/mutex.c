#include <sync/mutex.h>
#include <sched/thread.h>
#include <kassert.h>
#include <debug/panic.h>

void mutex_init(struct mutex *m, const char *name)
{
    spinlock_init(&m->lock, name);
    m->locked = false;
    m->owner = NULL;
    waitq_init(&m->wq, name);
}

void mutex_lock(struct mutex *m)
{
    struct thread *self = thread_current();
    spin_lock(&m->lock);
    if (m->locked && m->owner == self)
        panic("mutex %s: recursive lock by %s", m->lock.name, self->name);
    while (m->locked)
        waitq_wait(&m->wq, &m->lock);
    m->locked = true;
    m->owner = self;
    spin_unlock(&m->lock);
}

bool mutex_trylock(struct mutex *m)
{
    spin_lock(&m->lock);
    bool ok = !m->locked;
    if (ok) {
        m->locked = true;
        m->owner = thread_current();
    }
    spin_unlock(&m->lock);
    return ok;
}

void mutex_unlock(struct mutex *m)
{
    spin_lock(&m->lock);
    if (!m->locked || m->owner != thread_current())
        panic("mutex %s: unlock by non owner", m->lock.name);
    m->locked = false;
    m->owner = NULL;
    spin_unlock(&m->lock);
    waitq_wake_one(&m->wq);
}

bool mutex_held(struct mutex *m)
{
    spin_lock(&m->lock);
    bool held = m->locked && m->owner == thread_current();
    spin_unlock(&m->lock);
    return held;
}
