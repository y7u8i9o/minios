#include <tests/ktest.h>
#include <console.h>

/* M1: an exception produces a register dump. The panic that follows exits
 * QEMU with the failure code, the test harness checks the serial output.
 * The test needs only the exception vectors, so it runs early, before
 * memory management (and on an architecture in bring-up, A4). */
static void test_exception(void)
{
    kprintf("triggering an undefined instruction\n");
#if defined(__x86_64__)
    __asm__ volatile("ud2");
#elif defined(__aarch64__)
    __asm__ volatile("udf #0");
#endif
    ktest_fail("execution continued past the undefined instruction");
}
KTEST_DEFINE_STAGE("exception", test_exception, KTEST_EARLY);
