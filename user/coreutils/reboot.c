/* reboot: ask init to reboot the system. */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

int main(void)
{
    if (kill(1, SIGUSR2) < 0) {
        fprintf(stderr, "reboot: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
