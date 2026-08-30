/* shutdown: ask init to power the system off. -r reboots. */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int sig = argc > 1 && strcmp(argv[1], "-r") == 0 ? SIGUSR2 : SIGUSR1;
    if (kill(1, sig) < 0) {
        fprintf(stderr, "shutdown: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
