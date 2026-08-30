#include <drivers/debugexit.h>
#include <arch/io.h>

#define DEBUGEXIT_PORT 0xf4

void debugexit_exit(int code)
{
    outl(DEBUGEXIT_PORT, (uint32_t)code);
}
