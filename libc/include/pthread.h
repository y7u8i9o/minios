#pragma once
/* POSIX threads on the kernel's user threads (M35): threads with their
 * own stacks and thread local storage, futex based mutexes and
 * condition variables, keys, once controls, spin locks and read-write
 * locks. */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

struct pthread;
typedef struct pthread *pthread_t;

#define PTHREAD_STACK_MIN 16384
#define PTHREAD_STACK_DEFAULT (256 * 1024)
#define PTHREAD_KEYS_MAX 64
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

typedef struct {
    size_t stack_size;
    void *stack_addr;
    int detach_state;
} pthread_attr_t;

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

/* state: 0 unlocked, 1 locked, 2 locked with waiters. */
typedef struct {
    int state;
    int type;
    int owner;                  /* thread id for recursive and error checking mutexes */
    int count;                  /* recursion depth */
} pthread_mutex_t;

typedef struct {
    int type;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_NORMAL, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0 }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_ERRORCHECK, 0, 0 }

/* seq changes on every signal; waiters sleep on it. */
typedef struct {
    int seq;
    int waiters;
} pthread_cond_t;

typedef struct {
    int clock;
} pthread_condattr_t;

#define PTHREAD_COND_INITIALIZER { 0, 0 }

typedef unsigned pthread_key_t;

typedef struct {
    int state;
} pthread_once_t;

#define PTHREAD_ONCE_INIT { 0 }

typedef struct {
    int locked;
} pthread_spinlock_t;

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t readers_done, writer_done;
    int readers;                /* active readers */
    int writer;                 /* an active writer */
    int writers_waiting;
} pthread_rwlock_t;

typedef struct {
    int unused;
} pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 }

/* ---- threads ---- */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **result);
int pthread_detach(pthread_t thread);
__attribute__((noreturn)) void pthread_exit(void *result);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_yield(void);
/* The kernel thread id of a thread (gettid() for the caller). */
int pthread_tid(pthread_t thread);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state);

/* ---- mutexes ---- */
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);

/* ---- condition variables ---- */
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
/* deadline is on CLOCK_REALTIME; ETIMEDOUT when it passes. */
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);

/* ---- thread specific data and once ---- */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
int pthread_once(pthread_once_t *once, void (*init)(void));

/* ---- spin locks and read-write locks ---- */
int pthread_spin_init(pthread_spinlock_t *lock, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *lock);
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_trylock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);
int pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *lock);
int pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock);
int pthread_rwlock_wrlock(pthread_rwlock_t *lock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *lock);
int pthread_rwlock_unlock(pthread_rwlock_t *lock);
