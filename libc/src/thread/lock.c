/* The internal recursive lock, the futex wrappers and the main thread's
 * control block. */
#include "tcb.h"
#include <minios/syscall.h>
#include <sys/audio.h>
#include <unistd.h>
#include <errno.h>

static struct pthread main_thread;

long __futex_wait(int *addr, int value, unsigned long timeout_ms)
{
    return __syscall6(SYS_futex, (long)addr, FUTEX_WAIT, value, (long)timeout_ms, 0, 0);
}

long __futex_wake(int *addr, int count)
{
    return __syscall6(SYS_futex, (long)addr, FUTEX_WAKE, count, 0, 0, 0);
}

void __pthread_init_main(void)
{
    main_thread.self = &main_thread;
    main_thread.tid = (int)__syscall6(SYS_gettid, 0, 0, 0, 0, 0, 0);
    __syscall6(SYS_set_tls, (long)&main_thread, 0, 0, 0, 0, 0);
}

/* Acquire: uncontended is one atomic exchange; a contended thread marks
 * the lock as having waiters and sleeps until an unlock wakes it. */
void __libc_lock_lock(struct __libc_lock *l)
{
    int tid = __pthread_current()->tid;
    if (l->count && l->owner == tid) {
        l->count++;
        return;
    }
    int expected = 0;
    if (!__atomic_compare_exchange_n(&l->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        /* Contended: mark the lock as having a waiter and sleep until an
         * unlock hands it over; the mark stays until a holder unlocks
         * with no one left. */
        int c = expected;
        if (c != 2)
            c = __atomic_exchange_n(&l->state, 2, __ATOMIC_ACQUIRE);
        while (c != 0) {
            __futex_wait(&l->state, 2, 0);
            c = __atomic_exchange_n(&l->state, 2, __ATOMIC_ACQUIRE);
        }
    }
    l->owner = tid;
    l->count = 1;
}

void __libc_lock_unlock(struct __libc_lock *l)
{
    if (--l->count > 0)
        return;
    l->owner = -1;
    if (__atomic_exchange_n(&l->state, 0, __ATOMIC_RELEASE) == 2)
        __futex_wake(&l->state, 1);
}

int *__errno_location(void)
{
    return &__pthread_current()->errno_value;
}
