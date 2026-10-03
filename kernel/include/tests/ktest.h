#pragma once
#include <kernel.h>

/* Kernel self tests, compiled in with CONFIG_TESTS=1 and selected by the
 * `test=<name>` command line argument. A test that returns passes. */
/* The point of the start-up sequence (init/main.c) at which a test runs:
 * the earliest one at which what the test uses is initialized. A test
 * that runs before the scheduler also runs on an architecture whose port
 * has not reached the rest of the sequence (docs/plan/arm64.md). */
enum ktest_stage {
    KTEST_KINIT,                /* in the first thread, after the root is mounted */
    KTEST_EARLY,                /* after the boot environment is logged: console, log, command line */
    KTEST_MEMORY,               /* after the slab allocator: physical, virtual and heap memory */
    KTEST_TIMER,                /* after the timer, with interrupts enabled for the test */
};

struct ktest {
    const char *name;
    void (*fn)(void);
    enum ktest_stage stage;
};

/* The entries form an array between __ktests_start and __ktests_end. An
 * explicit alignment prevents the compiler from padding the 24 byte entries
 * to a larger alignment, which would leave gaps in that array. */
#define KTEST_DEFINE_STAGE(testname, func, when) \
    static const struct ktest __ktest_##func __used __section(".ktests") __aligned(8) = { testname, func, when }
#define KTEST_DEFINE(testname, func) KTEST_DEFINE_STAGE(testname, func, KTEST_KINIT)

/* Run the test selected on the command line if it belongs to stage.
 * Returns if none is selected or the selected test belongs to another
 * stage; a test that runs ends the boot. */
void ktest_run_stage(enum ktest_stage stage);
/* Run the test selected on the command line, if any. Returns if none. */
void ktest_run_selected(void);
__noreturn void ktest_pass(void);
__noreturn void ktest_fail(const char *fmt, ...) __printf(1, 2);

struct proc;

/* Keyboard helpers shared by typed session tests (tests/test_shell.c). */
void type_line(const char *s);
void type_ctrl(char c);
/* Start init as process 1 with the console login in place of the greeter
 * (tests/test_shell.c). */
struct proc *ktest_start_init(void);

#define ktest_assert(cond, ...)                          \
    do {                                                 \
        if (!(cond))                                     \
            ktest_fail(__VA_ARGS__);                     \
    } while (0)
