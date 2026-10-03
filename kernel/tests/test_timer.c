#include <tests/ktest.h>
#include <drivers/timer.h>
#include <drivers/ps2kbd.h>
#include <drivers/tty.h>
#include <arch/cpu.h>
#include <console.h>

/* M6: the timer ticks at 1000 Hz and sleep_ms waits accordingly. Runs
 * right after timer_init, with interrupts enabled. */
static void test_timer(void)
{
    ktest_assert(arch_irqs_enabled(), "interrupts are disabled");
    uint64_t i0 = timer_interrupts();
    uint64_t t0 = timer_ticks();
    sleep_ms(50);
    uint64_t t1 = timer_ticks();
    ktest_assert(t1 - t0 >= 50, "sleep_ms(50) returned after %lu ticks", t1 - t0);
    ktest_assert(t1 - t0 < 500, "sleep_ms(50) took %lu ticks", t1 - t0);
    /* The tick interrupt arrived during the sleep. */
    ktest_assert(timer_interrupts() - i0 >= 10, "%lu timer interrupts in 50 ms",
                 timer_interrupts() - i0);

    /* Ten short sleeps, counting elapsed ticks. */
    uint64_t start = timer_ticks();
    for (int i = 0; i < 10; i++)
        sleep_ms(10);
    uint64_t elapsed = timer_ticks() - start;
    ktest_assert(elapsed >= 100 && elapsed < 1000, "10 x sleep_ms(10) took %lu ticks", elapsed);
    kprintf("timer: %lu ticks for 10 x 10 ms, uptime %lu ms\n", elapsed, timer_ms());
}
KTEST_DEFINE_STAGE("timer", test_timer, KTEST_TIMER);

/* M6: scancode translation, modifiers and the line discipline, through
 * the input core and its console keyboard handler. */
static void feed(const uint8_t *codes, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ps2kbd_feed_scancode(codes[i]);
}

static void expect_line(const char *line)
{
    for (const char *p = line; *p; p++) {
        int c = tty_getc(&console_tty);
        ktest_assert(c == (uint8_t)*p, "expected '%c' (%d), got %d", *p, *p, c);
    }
}

static void test_kbd(void)
{
    ktest_assert(tty_getc(&console_tty) == -1, "buffer not empty at start");

    /* Shift+h, i, Enter -> "Hi\n" */
    static const uint8_t s1[] = { 0x2a, 0x23, 0xa3, 0xaa, 0x17, 0x97, 0x1c, 0x9c };
    feed(s1, sizeof s1);
    ktest_assert(tty_available(&console_tty) == 3, "available %zu, expected 3", tty_available(&console_tty));
    expect_line("Hi\n");

    /* Incomplete line is not readable, backspace erases, then completed.
     * Every key is released: the input core drops a press of a key that
     * is still down and repeats a pressed key. */
    static const uint8_t s2[] = { 0x1e, 0x9e, 0x30, 0xb0, 0x0e, 0x8e, 0x2e, 0xae };   /* a b <bs> c */
    feed(s2, sizeof s2);
    ktest_assert(tty_getc(&console_tty) == -1, "partial line readable");
    static const uint8_t s3[] = { 0x1c, 0x9c };
    feed(s3, sizeof s3);
    expect_line("ac\n");

    /* Control C discards the line typed so far (M15), caps lock
     * uppercases, extended prefix ignored. */
    static const uint8_t s4[] = { 0x10, 0x90, 0x1d, 0x2e, 0xae, 0x9d, 0x3a, 0xba, 0x2d, 0xad, 0x3a, 0xba,
                                  0xe0, 0x48, 0xe0, 0xc8, 0x02, 0x82, 0x1c, 0x9c };
    feed(s4, sizeof s4);
    expect_line("X1\n");
    ktest_assert(tty_getc(&console_tty) == -1, "buffer not empty at end");
}
KTEST_DEFINE("kbd", test_kbd);
