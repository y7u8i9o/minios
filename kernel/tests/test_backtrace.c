#include <tests/ktest.h>
#include <console.h>

/* M2: a page fault deep in a call chain yields a symbolized backtrace. Each
 * function is retained out of line and reads through a volatile pointer so the
 * optimizer cannot collapse the chain. The faulting function continues to work
 * after the load so the fault happens while its frame is still set up, and
 * the address is canonical so the CPU raises #PF rather than #GP. */
static volatile uint64_t *bad_pointer = (volatile uint64_t *)0xfffffffe00000000UL;
static volatile uint64_t sink;

void test_bt_a(int depth);
void test_bt_b(int depth);
void test_bt_c(int depth);

__noinline void test_bt_c(int depth)
{
    kprintf("test_bt_c: depth %d, faulting\n", depth);
    sink = *bad_pointer;
    kprintf("test_bt_c: survived the fault, value %lu\n", sink);
}

__noinline void test_bt_b(int depth)
{
    test_bt_c(depth + 1);
    sink = 0;
}

__noinline void test_bt_a(int depth)
{
    test_bt_b(depth + 1);
    sink = 1;
}

static void test_backtrace(void)
{
    test_bt_a(1);
    ktest_fail("execution continued past the page fault");
}
KTEST_DEFINE("backtrace", test_backtrace);
