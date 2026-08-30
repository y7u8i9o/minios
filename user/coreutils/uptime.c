/* uptime: time since boot and the processor count. */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    long ms = uptime_ms();
    long s = ms / 1000;
    printf("up %ld:%02ld:%02ld, %d cpu%s\n", s / 3600, (s / 60) % 60, s % 60,
           nproc(), nproc() == 1 ? "" : "s");
    return 0;
}
