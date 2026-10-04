/* Mutexes, condition variables, spin locks and read-write locks (M35).
 * The mutex is the three state futex mutex: 0 free, 1 locked, 2 locked with
 * a possible waiter; an uncontended lock and unlock are one atomic
 * operation each and only the contended paths enter the kernel. */
#include "tcb.h"
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>

/* Milliseconds until an absolute CLOCK_REALTIME deadline, at least 1; 0
 * when it has passed. */
static unsigned long ms_until(const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t ms = (deadline->tv_sec - now.tv_sec) * 1000 + (deadline->tv_nsec - now.tv_nsec) / 1000000;
    if (ms <= 0)
        return 0;
    return (unsigned long)ms + 1;
}

/* ---- mutexes ---- */

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    attr->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
    *type = attr->type;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    m->state = 0;
    m->type = attr ? attr->type : PTHREAD_MUTEX_DEFAULT;
    m->owner = 0;
    m->count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    return __atomic_load_n(&m->state, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

/* The contended path: mark the mutex as having a waiter, sleep until an
 * unlock hands it over. c is the state the failed acquisition saw.
 * deadline NULL waits without limit. */
static int mutex_lock_slow(pthread_mutex_t *m, int c, const struct timespec *deadline)
{
    if (c != 2)
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        unsigned long timeout = 0;
        if (deadline) {
            timeout = ms_until(deadline);
            if (timeout == 0)
                return ETIMEDOUT;
        }
        long r = __futex_wait(&m->state, 2, timeout);
        if (r == -ETIMEDOUT)
            return ETIMEDOUT;
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    }
    return 0;
}

static int mutex_lock_common(pthread_mutex_t *m, const struct timespec *deadline)
{
    int tid = __pthread_current()->tid;
    if (m->type != PTHREAD_MUTEX_NORMAL && m->count && m->owner == tid) {
        if (m->type == PTHREAD_MUTEX_ERRORCHECK)
            return EDEADLK;
        m->count++;
        return 0;
    }
    int expected = 0;
    if (!__atomic_compare_exchange_n(&m->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        int r = mutex_lock_slow(m, expected, deadline);
        if (r)
            return r;
    }
    m->owner = tid;
    m->count = 1;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    return mutex_lock_common(m, NULL);
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *deadline)
{
    return mutex_lock_common(m, deadline);
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    int tid = __pthread_current()->tid;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->count && m->owner == tid) {
        m->count++;
        return 0;
    }
    int expected = 0;
    if (!__atomic_compare_exchange_n(&m->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return EBUSY;
    m->owner = tid;
    m->count = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (!m->count || m->owner != __pthread_current()->tid)
            return EPERM;
        if (--m->count > 0)
            return 0;
    } else {
        m->count = 0;
    }
    m->owner = 0;
    if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2)
        __futex_wake(&m->state, 1);
    return 0;
}

/* ---- condition variables ---- */

int pthread_condattr_init(pthread_condattr_t *attr)
{
    attr->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    c->seq = 0;
    c->waiters = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    return __atomic_load_n(&c->waiters, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

/* Record the sequence number, release the mutex, sleep until the
 * sequence changes (or the deadline passes), take the mutex again. A
 * signal between the unlock and the sleep is not lost: the sequence has
 * changed and the futex wait returns at once. */
static int cond_wait_common(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline)
{
    int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    __atomic_add_fetch(&c->waiters, 1, __ATOMIC_ACQ_REL);
    int saved_count = m->count;
    m->count = 1;                   /* a recursive mutex is released fully */
    pthread_mutex_unlock(m);
    int r = 0;
    for (;;) {
        unsigned long timeout = 0;
        if (deadline) {
            timeout = ms_until(deadline);
            if (timeout == 0) {
                r = ETIMEDOUT;
                break;
            }
        }
        long w = __futex_wait(&c->seq, seq, timeout);
        if (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) != seq)
            break;
        if (w == -ETIMEDOUT) {
            r = ETIMEDOUT;
            break;
        }
    }
    __atomic_sub_fetch(&c->waiters, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(m);
    m->count = saved_count;
    return r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return cond_wait_common(c, m, NULL);
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline)
{
    return cond_wait_common(c, m, deadline);
}

int pthread_cond_signal(pthread_cond_t *c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&c->waiters, __ATOMIC_ACQUIRE))
        __futex_wake(&c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&c->waiters, __ATOMIC_ACQUIRE))
        __futex_wake(&c->seq, 0x7fffffff);
    return 0;
}

/* ---- spin locks ---- */

int pthread_spin_init(pthread_spinlock_t *l, int pshared)
{
    l->locked = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *l)
{
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t *l)
{
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            __arch_spin_hint();
    }
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *l)
{
    return __atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

int pthread_spin_unlock(pthread_spinlock_t *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    return 0;
}

/* ---- read-write locks: a mutex and two conditions, writers preferred ---- */

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr)
{
    pthread_mutex_init(&l->lock, NULL);
    pthread_cond_init(&l->readers_done, NULL);
    pthread_cond_init(&l->writer_done, NULL);
    l->readers = 0;
    l->writer = 0;
    l->writers_waiting = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    return l->readers || l->writer ? EBUSY : 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    while (l->writer || l->writers_waiting)
        pthread_cond_wait(&l->writer_done, &l->lock);
    l->readers++;
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    int busy = l->writer || l->writers_waiting;
    if (!busy)
        l->readers++;
    pthread_mutex_unlock(&l->lock);
    return busy ? EBUSY : 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    l->writers_waiting++;
    while (l->writer || l->readers)
        pthread_cond_wait(&l->readers_done, &l->lock);
    l->writers_waiting--;
    l->writer = 1;
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    int busy = l->writer || l->readers;
    if (!busy)
        l->writer = 1;
    pthread_mutex_unlock(&l->lock);
    return busy ? EBUSY : 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    if (l->writer) {
        l->writer = 0;
        pthread_cond_broadcast(&l->writer_done);
        pthread_cond_signal(&l->readers_done);
    } else if (l->readers > 0) {
        if (--l->readers == 0)
            pthread_cond_signal(&l->readers_done);
    }
    pthread_mutex_unlock(&l->lock);
    return 0;
}
