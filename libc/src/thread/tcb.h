#pragma once
/* libc thread internals (M35). Every thread has a control block whose
 * address is its FS base: the self pointer sits at %fs:0, so
 * pthread_self and errno are one load away. The first two words are the
 * struct dl_tcb the loader knows (minios/dl.h); the thread local storage
 * of the loaded objects lies below the block. */
#include <pthread.h>
#include <stdint.h>
#include <libc_arch.h>

struct dl_dtv;

struct pthread {
    struct pthread *self;           /* %fs:0 */
    struct dl_dtv *dtv;             /* %fs:8, the blocks of objects loaded by dlopen */
    int tid;                        /* kernel thread id */
    int main;                       /* the initial thread */
    int errno_value;
    void *(*start)(void *);
    void *arg;
    void *result;
    void *mapping;                  /* stack and control block, NULL for the main thread */
    size_t mapping_size;
    int detached;                   /* protected by __threads_lock */
    int exited;                     /* protected by __threads_lock */
    const void *keys[PTHREAD_KEYS_MAX];
    char sgr_sequence[16];          /* the buffer term_sgr returns */
    struct pthread *next;           /* __threads list, protected by __threads_lock */
};

/* A recursive lock for libc's own structures (malloc, stdio, the thread
 * list): futex based, so a contended thread sleeps. */
struct __libc_lock {
    int state;                      /* 0 free, 1 held, 2 held with waiters */
    int owner;                      /* tid */
    int count;
};

#define __LIBC_LOCK_INIT { 0, 0, 0 }

static inline struct pthread *__pthread_current(void)
{
    return __arch_thread_pointer();
}

void __libc_lock_lock(struct __libc_lock *l);
void __libc_lock_unlock(struct __libc_lock *l);
/* The raw futex operations; return the kernel's result. */
long __futex_wait(int *addr, int value, unsigned long timeout_ms);
long __futex_wake(int *addr, int count);
/* Install the control block of the main thread; run before anything
 * touches errno, after __tls_init. */
void __pthread_init_main(void);
/* Thread local storage (tls.c): the space below a control block, its
 * initialization for a new thread and its release. */
void __tls_init(const uintptr_t *aux);
size_t __tls_reserve(size_t *align);
void __tls_setup(struct pthread *t);
void __tls_free(struct pthread *t);
