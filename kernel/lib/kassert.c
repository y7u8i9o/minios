#include <kassert.h>
#include <debug/panic.h>

__noreturn void kassert_fail(const char *expr, const char *file, int line, const char *func)
{
    panic("assertion failed: %s at %s:%d in %s()", expr, file, line, func);
}
