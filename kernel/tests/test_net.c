/* Network stack self tests (NETWORK_PLAN.md): the controlled clock (N00),
 * the socket layer (N01) and the packet core with its worker (N02). They
 * run on the loopback interface and the kernel API alone; no device and
 * no user program is involved. */
#include <tests/ktest.h>
#include <net/clock.h>
#include <drivers/timer.h>
#include <console.h>

/* ---- N00: the controlled clock ---- */

static void test_net_clock(void)
{
    ktest_assert(!net_clock_is_controlled(), "the clock starts out uncontrolled");
    uint64_t real = timer_ms();
    uint64_t net = net_clock_ms();
    ktest_assert(net >= real && net - real < 50, "uncontrolled clock follows timer_ms: %lu vs %lu", net, real);

    net_clock_control(true);
    ktest_assert(net_clock_is_controlled(), "control taken");
    uint64_t frozen = net_clock_ms();
    sleep_ms(20);
    ktest_assert(net_clock_ms() == frozen, "controlled clock does not move on its own: %lu vs %lu",
                 net_clock_ms(), frozen);
    net_clock_advance(1000);
    ktest_assert(net_clock_ms() == frozen + 1000, "advance adds: %lu", net_clock_ms());
    net_clock_set(5);
    ktest_assert(net_clock_ms() == 5, "set replaces: %lu", net_clock_ms());
    net_clock_advance(7);
    ktest_assert(net_clock_ms() == 12, "advance after set: %lu", net_clock_ms());

    net_clock_control(false);
    ktest_assert(!net_clock_is_controlled(), "control released");
    net_clock_set(99);
    net_clock_advance(99);
    real = timer_ms();
    net = net_clock_ms();
    ktest_assert(net >= real && net - real < 50, "released clock follows timer_ms again: %lu vs %lu", net, real);
    kprintf("net_clock: ok\n");
}
KTEST_DEFINE("net_clock", test_net_clock);
