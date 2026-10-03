/* POSIX threads (M35): creation, joining, detaching, keys and once. A
 * thread's stack, thread local storage and control block are one
 * anonymous mapping; the block sits at its top, the storage of the
 * loaded objects directly below it, and the block becomes the thread's
 * FS base before any user code runs. Detached threads that have exited are reclaimed by the next
 * pthread_create or pthread_join, since a thread cannot unmap the stack
 * it is running on. */
#include "tcb.h"
#include <minios/syscall.h>
#include <sys/mman.h>
#include <sys/thread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Protects the thread list, the detached and exited flags and the key
 * table. */
static struct __libc_lock threads_lock = __LIBC_LOCK_INIT;
static struct pthread *threads;

static void (*key_destructors[PTHREAD_KEYS_MAX])(void *);
static unsigned char key_used[PTHREAD_KEYS_MAX];

static void unlink_thread(struct pthread *t)
{
    for (struct pthread **p = &threads; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            return;
        }
}

/* Free the mappings of exited detached threads. The caller has acquired threads_lock. */
static void reap_detached(void)
{
    for (struct pthread *t = threads; t;) {
        struct pthread *next = t->next;
        if (t->detached && t->exited) {
            unlink_thread(t);
            thread_join(t->tid, NULL);      /* the kernel side is finished or about to be */
            if (t->mapping)
                munmap(t->mapping, t->mapping_size);
        }
        t = next;
    }
}

static void run_key_destructors(struct pthread *self)
{
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
        int any = 0;
        for (unsigned k = 0; k < PTHREAD_KEYS_MAX; k++) {
            const void *value = self->keys[k];
            if (!value || !key_destructors[k])
                continue;
            self->keys[k] = NULL;
            key_destructors[k]((void *)value);
            any = 1;
        }
        if (!any)
            break;
    }
}

static void thread_entry(void *arg)
{
    struct pthread *self = arg;
    __syscall6(SYS_set_tls, (long)__tls_thread_pointer(self), 0, 0, 0, 0, 0);
    self->tid = (int)__syscall6(SYS_gettid, 0, 0, 0, 0, 0, 0);
    pthread_exit(self->start(self->arg));
}

int pthread_attr_init(pthread_attr_t *attr)
{
    attr->stack_size = PTHREAD_STACK_DEFAULT;
    attr->stack_addr = NULL;
    attr->detach_state = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
    if (size < PTHREAD_STACK_MIN)
        return EINVAL;
    attr->stack_size = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size)
{
    *size = attr->stack_size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state)
{
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    attr->detach_state = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state)
{
    *state = attr->detach_state;
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    pthread_attr_t defaults;
    if (!attr) {
        pthread_attr_init(&defaults);
        attr = &defaults;
    }
    __libc_lock_lock(&threads_lock);
    reap_detached();
    __libc_lock_unlock(&threads_lock);

    /* The area with the control block and the thread local storage at the
     * top of the mapping, aligned as the storage requires, the stack below
     * it. */
    size_t align;
    size_t area_size = __tls_area_size(&align);
    size_t size = (attr->stack_size + area_size + align + 4095) & ~(size_t)4095;
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return EAGAIN;
    unsigned char *area = (unsigned char *)(((uintptr_t)mapping + size - area_size) & ~(uintptr_t)(align - 1));
    struct pthread *t = __tls_area_place(area);
    memset(t, 0, sizeof *t);
    t->self = t;
    __tls_setup(t);
    t->start = start;
    t->arg = arg;
    t->mapping = mapping;
    t->mapping_size = size;
    t->detached = attr->detach_state == PTHREAD_CREATE_DETACHED;

    __libc_lock_lock(&threads_lock);
    t->next = threads;
    threads = t;
    thread_t tid;
    int r = thread_create(&tid, thread_entry, t, mapping, (size_t)(area - (unsigned char *)mapping));
    if (r < 0) {
        int error = errno;
        unlink_thread(t);
        __libc_lock_unlock(&threads_lock);
        munmap(mapping, size);
        return error ? error : EAGAIN;
    }
    t->tid = tid;
    __libc_lock_unlock(&threads_lock);
    *thread = t;
    return 0;
}

void pthread_exit(void *result)
{
    struct pthread *self = __pthread_current();
    self->result = result;
    run_key_destructors(self);
    __tls_free(self);
    if (self->main) {
        /* The main thread: the process lives on while other threads run
         * and ends with the last one. */
        for (;;)
            __syscall6(SYS_thread_exit, 0, 0, 0, 0, 0, 0);
    }
    __libc_lock_lock(&threads_lock);
    self->exited = 1;
    __libc_lock_unlock(&threads_lock);
    for (;;)
        __syscall6(SYS_thread_exit, 0, 0, 0, 0, 0, 0);
}

int pthread_join(pthread_t t, void **result)
{
    if (!t || t == __pthread_current())
        return EDEADLK;
    __libc_lock_lock(&threads_lock);
    int detached = t->detached;
    __libc_lock_unlock(&threads_lock);
    if (detached)
        return EINVAL;
    if (thread_join(t->tid, NULL) < 0)
        return errno == ESRCH ? ESRCH : errno;
    if (result)
        *result = t->result;
    __libc_lock_lock(&threads_lock);
    unlink_thread(t);
    reap_detached();
    __libc_lock_unlock(&threads_lock);
    munmap(t->mapping, t->mapping_size);
    return 0;
}

int pthread_detach(pthread_t t)
{
    if (!t)
        return EINVAL;
    __libc_lock_lock(&threads_lock);
    if (t->detached) {
        __libc_lock_unlock(&threads_lock);
        return EINVAL;
    }
    t->detached = 1;
    reap_detached();
    __libc_lock_unlock(&threads_lock);
    return 0;
}

pthread_t pthread_self(void)
{
    return __pthread_current();
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

int pthread_yield(void)
{
    return sched_yield();
}

int pthread_tid(pthread_t t)
{
    return t ? t->tid : -1;
}

/* ---- keys ---- */

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    __libc_lock_lock(&threads_lock);
    for (unsigned k = 0; k < PTHREAD_KEYS_MAX; k++) {
        if (!key_used[k]) {
            key_used[k] = 1;
            key_destructors[k] = destructor;
            __libc_lock_unlock(&threads_lock);
            *key = k;
            return 0;
        }
    }
    __libc_lock_unlock(&threads_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX)
        return EINVAL;
    __libc_lock_lock(&threads_lock);
    key_used[key] = 0;
    key_destructors[key] = NULL;
    __libc_lock_unlock(&threads_lock);
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX)
        return NULL;
    return (void *)__pthread_current()->keys[key];
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= PTHREAD_KEYS_MAX || !key_used[key])
        return EINVAL;
    __pthread_current()->keys[key] = value;
    return 0;
}

/* ---- once ---- */

/* state: 0 not run, 1 running, 2 done. Waiters sleep on the word. */
int pthread_once(pthread_once_t *once, void (*init)(void))
{
    int expected = 0;
    if (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) == 2)
        return 0;
    if (__atomic_compare_exchange_n(&once->state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init();
        __atomic_store_n(&once->state, 2, __ATOMIC_RELEASE);
        __futex_wake(&once->state, 0x7fffffff);
        return 0;
    }
    while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2)
        __futex_wait(&once->state, 1, 0);
    return 0;
}
