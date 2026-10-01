#include <tests/ktest.h>
#include <sync/mutex.h>
#include <sched/thread.h>
#include <drivers/timer.h>
#include <drivers/ps2kbd.h>
#include <console.h>

/* The hung task detector (debug/hung.c) reports a thread that waits for a
 * mutex longer than the limit, hung_task=2 on the command line of the
 * case, and Alt+SysRq prints the thread table. */

static struct mutex hung_mutex;

static void hung_waiter(void *arg)
{
    mutex_lock(&hung_mutex);
    mutex_unlock(&hung_mutex);
}

static void test_hung_task(void)
{
    mutex_init(&hung_mutex, "hung_test_mutex");
    mutex_lock(&hung_mutex);
    struct thread *t = thread_create("hung_waiter", hung_waiter, NULL, 0);
    ktest_assert(t != NULL, "cannot create the waiter");
    sleep_ms(4000);
    mutex_unlock(&hung_mutex);
    thread_join(t);
    kprintf("hung_task: waiter released\n");

    /* Alt down, SysRq down and up (e0 37, e0 b7), Alt up. */
    static const uint8_t keys[] = { 0x38, 0xe0, 0x37, 0xe0, 0xb7, 0xb8 };
    for (unsigned i = 0; i < sizeof keys; i++)
        ps2kbd_feed_scancode(keys[i]);
    sleep_ms(1500);
    kprintf("hung_task: ok\n");
}
KTEST_DEFINE("hung_task", test_hung_task);
