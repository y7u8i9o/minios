#include <tests/ktest.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <lib/cmdline.h>
#include <console.h>

/* M0/M1: the kernel reached its entry point and kprintf formats correctly. */
static void check(const char *expect, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ktest_assert(strcmp(buf, expect) == 0, "format \"%s\" gave \"%s\", expected \"%s\"",
                 fmt, buf, expect);
}

static void test_boot(void)
{
    check("42", "%d", 42);
    check("-42", "%d", -42);
    check("  42", "%4d", 42);
    check("0042", "%04d", 42);
    check("42  |", "%-4d|", 42);
    check("ff", "%x", 255);
    check("FF", "%X", 255);
    check("00ff", "%04x", 255);
    check("18446744073709551615", "%lu", (unsigned long)-1);
    check("-9223372036854775808", "%ld", (long)(-9223372036854775807L - 1));
    check("0x1000", "%p", (void *)0x1000);
    check("abc", "%s", "abc");
    check("  abc", "%5s", "abc");
    check("x", "%c", 'x');
    check("%", "%%");
    check("123", "%zu", (size_t)123);
    check("(null)", "%s", (char *)NULL);

    char val[16];
    ktest_assert(cmdline_lookup("test", val, sizeof val), "cmdline lookup failed");
    ktest_assert(strcmp(val, "boot") == 0, "cmdline value \"%s\"", val);
    ktest_assert(!cmdline_lookup("tes", val, sizeof val), "prefix matched as key");
    kprintf("kprintf works: %s %d %x\n", "ok", 1, 0xbeef);
}
KTEST_DEFINE_EARLY("boot", test_boot);
