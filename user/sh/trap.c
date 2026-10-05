/* trap: commands that the shell runs on a signal or at its exit (POSIX
 * trap, docs/design/sh.md).
 *
 *     trap [action condition...]
 *     trap - condition...
 *
 * A condition is EXIT (0), a signal name with or without SIG, or a signal
 * number. An empty action ignores the signal, and - restores its earlier
 * disposition. A caught signal only marks the trap. exec_node runs the
 * marked actions after the current command, with $? retained. The EXIT
 * action runs when the shell exits, in the process that set it and not in
 * the subshells forked from that process. */
#include "sh.h"
#include <signal.h>

#define NTRAPS NSIG

static const struct {
    const char *name;
    int sig;
} names[] = {
    {"EXIT", 0},     {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"ILL", SIGILL},
    {"ABRT", SIGABRT}, {"FPE", SIGFPE}, {"KILL", SIGKILL}, {"USR1", SIGUSR1}, {"SEGV", SIGSEGV},
    {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"CHLD", SIGCHLD},
    {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU},
    {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ}, {"WINCH", SIGWINCH},
};

static char *actions[NTRAPS];               /* NULL: the default disposition */
static struct sigaction original[NTRAPS];   /* the disposition before the first trap */
static int saved[NTRAPS];
static volatile sig_atomic_t pending[NTRAPS], any_pending;
static pid_t owner;
static int running;

static int signal_of(const char *spec)
{
    if (*spec >= '0' && *spec <= '9') {
        char *end;
        long n = strtol(spec, &end, 10);
        return *end || n >= NTRAPS ? -1 : (int)n;
    }
    if (!strncmp(spec, "SIG", 3))
        spec += 3;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(names[i].name, spec))
            return names[i].sig;
    return -1;
}

static const char *name_of(int sig)
{
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].sig == sig)
            return names[i].name;
    return NULL;
}

static void on_signal(int sig)
{
    pending[sig] = 1;
    any_pending = 1;
}

static void run_action(const char *action)
{
    int status = last_status;
    running = 1;
    run_line(action);
    running = 0;
    last_status = status;
}

static void run_exit_trap(void)
{
    if (getpid() != owner || !actions[0] || running)
        return;
    char *action = actions[0];
    actions[0] = NULL;
    if (*action)
        run_action(action);
    free(action);
    fflush(NULL);
}

void traps_run_pending(void)
{
    if (!any_pending || running)
        return;
    any_pending = 0;
    for (int sig = 1; sig < NTRAPS; sig++)
        if (pending[sig]) {
            pending[sig] = 0;
            if (actions[sig] && *actions[sig])
                run_action(actions[sig]);
        }
}

static void print_traps(void)
{
    for (int sig = 0; sig < NTRAPS; sig++) {
        if (!actions[sig])
            continue;
        const char *name = name_of(sig);
        printf("trap -- '");
        for (const char *p = actions[sig]; *p; p++)
            if (*p == '\'')
                fputs("'\\''", stdout);
            else
                putchar(*p);
        if (name)
            printf("' %s\n", name);
        else
            printf("' %d\n", sig);
    }
}

int builtin_trap(int argc, char **argv)
{
    if (argc == 1) {
        print_traps();
        return 0;
    }
    if (!owner) {
        owner = getpid();
        atexit(run_exit_trap);
    }
    /* An unsigned number as the first operand resets, as with -. */
    int first = 2;
    const char *action = argv[1];
    if (!strcmp(action, "-") || (argc == 2 && signal_of(action) >= 0)) {
        action = NULL;
        first = !strcmp(argv[1], "-") ? 2 : 1;
    }
    int result = 0;
    for (int i = first; i < argc; i++) {
        int sig = signal_of(argv[i]);
        if (sig < 0 || sig == SIGKILL || sig == SIGSTOP) {
            fprintf(stderr, "trap: %s: invalid signal\n", argv[i]);
            result = 1;
            continue;
        }
        free(actions[sig]);
        actions[sig] = action ? strdup(action) : NULL;
        if (sig == 0)
            continue;
        if (!saved[sig]) {
            sigaction(sig, NULL, &original[sig]);
            saved[sig] = 1;
        }
        if (!action) {
            sigaction(sig, &original[sig], NULL);
        } else {
            struct sigaction sa = {0};
            sa.sa_handler = *action ? on_signal : SIG_IGN;
            sigemptyset(&sa.sa_mask);
            sigaction(sig, &sa, NULL);
        }
    }
    return result;
}
