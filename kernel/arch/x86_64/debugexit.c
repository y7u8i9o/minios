#include <arch/platform.h>
#include <arch/io.h>

#define DEBUGEXIT_PORT 0xf4

void platform_test_exit(int code)
{
    outl(DEBUGEXIT_PORT, (uint32_t)code);
}
