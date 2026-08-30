/* kill: send a signal (default SIGTERM) to processes. kill [-SIG] pid... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

static const struct { const char *name; int sig; } names[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "KILL", SIGKILL }, { "USR1", SIGUSR1 },
    { "SEGV", SIGSEGV }, { "USR2", SIGUSR2 }, { "PIPE", SIGPIPE }, { "TERM", SIGTERM },
    { "CHLD", SIGCHLD },
};

int main(int argc, char **argv)
{
    int sig = SIGTERM, first = 1;
    if (argc > 1 && argv[1][0] == '-') {
        const char *s = argv[1] + 1;
        if (*s >= '0' && *s <= '9') {
            sig = atoi(s);
        } else {
            sig = -1;
            for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
                if (strcmp(names[i].name, s) == 0)
                    sig = names[i].sig;
            if (sig < 0) {
                fprintf(stderr, "kill: unknown signal %s\n", s);
                return 1;
            }
        }
        first = 2;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: kill [-SIG] pid...\n");
        return 1;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        if (kill(atoi(argv[i]), sig) < 0) {
            fprintf(stderr, "kill: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
