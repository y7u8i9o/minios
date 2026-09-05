#include <tests/ktest.h>
#include <ipc/pipe.h>
#include <fs/vfs.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <mm/slab.h>
#include <drivers/timer.h>
#include <console.h>

/* Both ends of many pipes released at the same moment on different CPUs,
 * as happens when the processes of a pipeline exit together. Whichever
 * end is released second must not touch the pipe after the first one
 * freed it: before the fix the first release notified pollers on freed
 * memory and spun forever or faulted. Two threads walk the same list of
 * pipes in step, one closing read ends and the other write ends. */
enum { PIPES = 256, ROUNDS = 40 };

struct close_job {
    struct file **ends;
    volatile int *go;
};

static void close_ends(void *arg)
{
    struct close_job *job = arg;
    /* Yield rather than spin: kernel threads are not preempted, and the
     * test thread that sets go may be queued on this CPU. */
    while (!__atomic_load_n(job->go, __ATOMIC_ACQUIRE))
        sched_yield();
    for (int i = 0; i < PIPES; i++)
        file_put(job->ends[i]);
}

static void test_pipe_close(void)
{
    struct file **rd = kmalloc(PIPES * sizeof *rd);
    struct file **wr = kmalloc(PIPES * sizeof *wr);
    ktest_assert(rd && wr, "end arrays");
    for (int round = 0; round < ROUNDS; round++) {
        for (int i = 0; i < PIPES; i++)
            ktest_assert(pipe_create(&rd[i], &wr[i]) == 0, "pipe %d", i);
        volatile int go = 0;
        struct close_job a = { rd, &go }, b = { wr, &go };
        struct thread *ta = thread_create("pipe_close_rd", close_ends, &a, 0);
        ktest_assert(ta != NULL, "thread a");
        struct thread *tb = thread_create("pipe_close_wr", close_ends, &b, 0);
        ktest_assert(tb != NULL, "thread b");
        sleep_ms(1);
        __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
        thread_join(ta);
        thread_join(tb);
    }
    kfree(rd);
    kfree(wr);
    kprintf("pipe_close: ok\n");
}
KTEST_DEFINE("pipe_close", test_pipe_close);
