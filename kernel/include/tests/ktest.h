#pragma once
#include <kernel.h>

/* Kernel self tests, compiled in with CONFIG_TESTS=1 and selected by the
 * `test=<name>` command line argument. A test that returns passes. */
struct ktest {
    const char *name;
    void (*fn)(void);
};

#define KTEST_DEFINE(testname, func) \
    static const struct ktest __ktest_##func __used __section(".ktests") = { testname, func }

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
