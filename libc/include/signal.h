#pragma once
#include <sys/types.h>
#include <minios/abi.h>

typedef void (*sighandler_t)(int);

int kill(pid_t pid, int sig);
int raise(int sig);
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
const char *strsignal(int sig);
