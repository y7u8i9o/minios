#pragma once
#include <kernel.h>

/* Kernel self tests, compiled in with CONFIG_TESTS=1 and selected by the
 * `test=<name>` command line argument. A test that returns passes. */
struct ktest {
    const char *name;
    void (*fn)(void);
    bool early;                 /* runs before memory management, see ktest_run_early */
};

/* The entries form an array between __ktests_start and __ktests_end. An
 * explicit alignment keeps the compiler from padding the 24 byte entries
 * to a larger alignment, which would leave gaps in that array. */
#define KTEST_DEFINE(testname, func) \
    static const struct ktest __ktest_##func __used __section(".ktests") __aligned(8) = { testname, func, false }
/* A test that needs only the console, the log and the command line. It
 * runs right after the boot environment is logged, before memory
 * management, so it also runs on an architecture whose port has not
 * reached the rest of the start-up sequence (docs/plan/arm64.md). */
#define KTEST_DEFINE_EARLY(testname, func) \
    static const struct ktest __ktest_##func __used __section(".ktests") __aligned(8) = { testname, func, true }

/* Run the test selected on the command line if it is an early test.
 * Returns if none is selected or the selected test is not early. */
void ktest_run_early(void);
/* Run the test selected on the command line, if any. Returns if none. */
void ktest_run_selected(void);
__noreturn void ktest_pass(void);
__noreturn void ktest_fail(const char *fmt, ...) __printf(1, 2);

/* Keyboard helpers shared by typed session tests (tests/test_shell.c). */
void type_line(const char *s);
void type_ctrl(char c);

#define ktest_assert(cond, ...)                          \
    do {                                                 \
        if (!(cond))                                     \
            ktest_fail(__VA_ARGS__);                     \
    } while (0)
