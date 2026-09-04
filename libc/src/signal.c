#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <minios/syscall.h>
#include <sys/reboot.h>

void __sigreturn_trampoline(void);

int kill(pid_t pid, int sig)
{
    return (int)syscall2(SYS_kill, pid, sig);
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    struct sigaction copy;
    if (act) {
        copy = *act;
        copy.sa_flags |= SA_RESTORER;
        copy.sa_restorer = __sigreturn_trampoline;
        act = &copy;
    }
    return (int)syscall3(SYS_sigaction, sig, act, oldact);
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction act, old;
    memset(&act, 0, sizeof act);
    act.sa_handler = handler;
    if (sigaction(sig, &act, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return (int)syscall3(SYS_sigprocmask, how, set, oldset);
}

int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set) { *set = ~0UL; return 0; }
int sigaddset(sigset_t *set, int sig) { *set |= 1UL << sig; return 0; }
int sigdelset(sigset_t *set, int sig) { *set &= ~(1UL << sig); return 0; }
int sigismember(const sigset_t *set, int sig) { return (*set >> sig) & 1; }

const char *strsignal(int sig)
{
    switch (sig) {
    case SIGHUP: return "Hangup";
    case SIGINT: return "Interrupt";
    case SIGQUIT: return "Quit";
    case SIGILL: return "Illegal instruction";
    case SIGABRT: return "Aborted";
    case SIGFPE: return "Floating point exception";
    case SIGKILL: return "Killed";
    case SIGUSR1: return "User defined signal 1";
    case SIGSEGV: return "Segmentation fault";
    case SIGUSR2: return "User defined signal 2";
    case SIGPIPE: return "Broken pipe";
    case SIGALRM: return "Alarm clock";
    case SIGTERM: return "Terminated";
    case SIGCHLD: return "Child exited";
    case SIGCONT: return "Continued";
    case SIGSTOP: return "Stopped (signal)";
    case SIGTSTP: return "Stopped";
    case SIGTTIN: return "Stopped (tty input)";
    case SIGTTOU: return "Stopped (tty output)";
    default: return "Unknown signal";
    }
}

int reboot(int cmd)
{
    return (int)syscall1(SYS_reboot, cmd);
}
