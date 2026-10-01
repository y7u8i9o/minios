/* The internal recursive lock, the futex wrappers and the main thread's
 * control block. */
#include "tcb.h"
#include <minios/syscall.h>
#include <sys/audio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

/* The main thread's struct pthread and control block when the program
 * has no thread local storage beyond them. */
static unsigned char main_area[sizeof(struct pthread) + 192] __attribute__((aligned(64)));

long __futex_wait(int *addr, int value, unsigned long timeout_ms)
{
    return __syscall6(SYS_futex, (long)addr, FUTEX_WAIT, value, (long)timeout_ms, 0, 0);
}

long __futex_wake(int *addr, int count)
{
    return __syscall6(SYS_futex, (long)addr, FUTEX_WAKE, count, 0, 0, 0);
}

/* The main thread's area is static unless the process has thread local
 * storage that does not fit, in which case it comes from one mapping laid
 * out like every other thread's. Raw system calls: errno has no home yet. */
void __pthread_init_main(void)
{
    size_t align;
    size_t size = __tls_area_size(&align);
    void *area = main_area;
    if (size > sizeof main_area || align > 64) {
        size_t bytes = (size + align + 4095) & ~(size_t)4095;
        long r = __syscall6(SYS_mmap, 0, (long)bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (r < 0) {
            static const char msg[] = "libc: cannot allocate thread local storage\n";
            __syscall6(SYS_write, 2, (long)msg, sizeof msg - 1, 0, 0, 0);
            __syscall6(SYS_exit, 127, 0, 0, 0, 0, 0);
        }
        area = (void *)(((uintptr_t)r + align - 1) & ~(uintptr_t)(align - 1));
    }
    struct pthread *t = __tls_area_place(area);
    t->self = t;
    t->main = 1;
    t->tid = (int)__syscall6(SYS_gettid, 0, 0, 0, 0, 0, 0);
    __tls_setup(t);
    __syscall6(SYS_set_tls, (long)__tls_thread_pointer(t), 0, 0, 0, 0, 0);
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
