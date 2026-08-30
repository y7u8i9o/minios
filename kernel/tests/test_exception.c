#include <tests/ktest.h>
#include <console.h>

/* M1: an exception produces a register dump. The panic that follows exits
 * QEMU with the failure code, the test harness checks the serial output. */
static void test_exception(void)
{
    kprintf("triggering #UD\n");
    __asm__ volatile("ud2");
    ktest_fail("execution continued past ud2");
}
KTEST_DEFINE("exception", test_exception);
