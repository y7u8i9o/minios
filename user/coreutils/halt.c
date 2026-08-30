/* halt: ask init to stop the machine without powering it off. */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

int main(void)
{
    if (kill(1, SIGHUP) < 0) {
        fprintf(stderr, "halt: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
