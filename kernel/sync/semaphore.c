#include <sync/semaphore.h>

void semaphore_init(struct semaphore *s, int count, const char *name)
{
    spinlock_init(&s->lock, name);
    s->count = count;
    waitq_init(&s->wq, name);
}

void semaphore_down(struct semaphore *s)
{
    spin_lock(&s->lock);
    while (s->count <= 0)
        waitq_wait(&s->wq, &s->lock);
    s->count--;
    spin_unlock(&s->lock);
}

bool semaphore_trydown(struct semaphore *s)
{
    spin_lock(&s->lock);
    bool ok = s->count > 0;
    if (ok)
        s->count--;
    spin_unlock(&s->lock);
    return ok;
}

void semaphore_up(struct semaphore *s)
{
    spin_lock(&s->lock);
    s->count++;
    spin_unlock(&s->lock);
    waitq_wake_one(&s->wq);
}
