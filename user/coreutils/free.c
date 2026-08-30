/* free: memory and swap usage from /dev/meminfo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    FILE *f = fopen("/dev/meminfo", "r");
    if (!f) {
        perror("free: /dev/meminfo");
        return 1;
    }
    long total = 0, avail = 0, stotal = 0, sfree = 0;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        long v = 0;
        char *colon = strchr(line, ':');
        if (colon)
            v = strtol(colon + 1, NULL, 10);
        if (strncmp(line, "MemTotal", 8) == 0) total = v;
        else if (strncmp(line, "MemFree", 7) == 0) avail = v;
        else if (strncmp(line, "SwapTotal", 9) == 0) stotal = v;
        else if (strncmp(line, "SwapFree", 8) == 0) sfree = v;
    }
    fclose(f);
    printf("%-8s %10s %10s %10s\n", "", "total", "used", "free");
    printf("%-8s %10ld %10ld %10ld\n", "Mem:", total, total - avail, avail);
    printf("%-8s %10ld %10ld %10ld\n", "Swap:", stotal, stotal - sfree, sfree);
    printf("(values in kB)\n");
    return 0;
}
