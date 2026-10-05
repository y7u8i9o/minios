/* M15 test: signal delivery, handlers, masks, defaults and process
 * groups. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static volatile int got_usr1, got_chld, got_usr2, got_segv;

static void on_usr1(int sig) { got_usr1 += sig == SIGUSR1; }
static void on_usr2(int sig) { got_usr2++; }
static void on_chld(int sig) { got_chld++; }
static void on_segv(int sig)
{
    got_segv = 1;
    _exit(42);
}

static int child_status(pid_t pid)
{
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return status;
}

int main(void)
{
    printf("sigtest: pid %d\n", getpid());

    /* A handler runs and returns through sigreturn with state intact. */
    CHECK(signal(SIGUSR1, on_usr1) == SIG_DFL, "signal returns old disposition");
    volatile long retain = 0x1234567;
    CHECK(raise(SIGUSR1) == 0 && got_usr1 == 1 && retain == 0x1234567, "raise handled %d", got_usr1);
    CHECK(signal(SIGUSR1, on_usr1) == on_usr1, "old handler reported");

    /* Blocking defers delivery until unblocked. */
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0 && old == 0, "block");
    raise(SIGUSR1);
    CHECK(got_usr1 == 1, "blocked signal not delivered");
    CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0 && got_usr1 == 2, "delivered after unblock: %d", got_usr1);

    /* Default actions in a child. */
    pid_t pid = fork();
    if (pid == 0) {
        for (;;)
            sched_yield();
    }
    CHECK(kill(pid, SIGTERM) == 0, "kill TERM");
    int status = child_status(pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "TERM default status 0x%x", status);

    /* A child that ignores SIGTERM survives it. SIGKILL cannot be ignored.
     * The child reports through a pipe that it has set the dispositions,
     * because with several CPUs the parent may otherwise send SIGTERM
     * before the child calls signal. */
    int ready[2];
    CHECK(pipe(ready) == 0, "pipe");
    pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_IGN);
        signal(SIGKILL, SIG_IGN);
        write(ready[1], "r", 1);
        for (;;)
            sched_yield();
    }
    char byte;
    CHECK(read(ready[0], &byte, 1) == 1, "child ready");
    close(ready[0]);
    close(ready[1]);
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++)
        sched_yield();
    CHECK(kill(pid, 0) == 0, "still alive after ignored TERM");
    kill(pid, SIGKILL);
    status = child_status(pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "KILL status 0x%x", status);

    /* SIGCHLD reaches the parent. */
    signal(SIGCHLD, on_chld);
    pid = fork();
    if (pid == 0)
        _exit(0);
    child_status(pid);
    CHECK(got_chld == 1, "SIGCHLD count %d", got_chld);
    signal(SIGCHLD, SIG_DFL);

    /* A signal interrupts a blocking read on a pipe with EINTR. */
    int p[2];
    pipe(p);
    signal(SIGUSR2, on_usr2);
    pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 50; i++)
            sched_yield();
        kill(getppid(), SIGUSR2);
        _exit(0);
    }
    char buf[8];
    ssize_t n = read(p[0], buf, sizeof buf);
    CHECK(n < 0 && errno == EINTR && got_usr2 == 1, "read interrupted: %ld %s", (long)n, strerror(errno));
    child_status(pid);
    close(p[0]);
    close(p[1]);

    /* SIGSEGV handler catches a bad access. */
    pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, on_segv);
        *(volatile int *)0 = 1;
        _exit(1);
    }
    status = child_status(pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42, "SEGV handler status 0x%x", status);

    /* SIGPIPE terminates a writer without readers by default. The read end
     * is closed before the fork. Otherwise a child that runs first writes
     * while the parent still has a reader. */
    pipe(p);
    close(p[0]);
    pid = fork();
    if (pid == 0) {
        write(p[1], "x", 1);
        _exit(1);
    }
    close(p[1]);
    status = child_status(pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE, "SIGPIPE status 0x%x", status);

    /* Handlers are reset by exec, ignored signals remain ignored. */
    signal(SIGTERM, SIG_IGN);
    pid = fork();
    if (pid == 0) {
        char *const args[] = { "cat", NULL };
        int q[2];
        pipe(q);
        dup2(q[0], 0);
        execvp("cat", args);
        _exit(127);
    }
    for (int i = 0; i < 50; i++)
        sched_yield();
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; i++)
        sched_yield();
    kill(pid, SIGINT);
    status = child_status(pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT, "ignore survives exec, INT kills: 0x%x", status);
    signal(SIGTERM, SIG_DFL);

    /* Process groups. */
    pid_t g1 = fork();
    if (g1 == 0) {
        setpgid(0, 0);
        for (;;)
            sched_yield();
    }
    pid_t g2 = fork();
    if (g2 == 0) {
        for (int i = 0; i < 10; i++)
            sched_yield();
        setpgid(0, g1);
        for (;;)
            sched_yield();
    }
    setpgid(g1, g1);
    setpgid(g2, g1);
    CHECK(getpgid(g1) == g1 && getpgid(g2) == g1 && getpgid(0) == getpid(), "pgids %d %d %d", getpgid(g1), getpgid(g2), getpgid(0));
    CHECK(kill(-g1, SIGTERM) == 0, "kill group");
    status = child_status(g1);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "g1 status 0x%x", status);
    status = child_status(g2);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "g2 status 0x%x", status);

    /* init survives default actions. */
    CHECK(kill(1, SIGTERM) == 0 && kill(1, SIGKILL) == 0 && kill(1, 0) == 0, "init protected");

    /* Errors. */
    CHECK(kill(99999, SIGTERM) < 0 && errno == ESRCH, "ESRCH");
    CHECK(kill(getpid(), 0) == 0, "signal 0");
    CHECK(signal(SIGKILL, on_usr1) == SIG_ERR && errno == EINVAL, "SIGKILL handler refused");
    CHECK(kill(getpid(), 40) < 0 && errno == EINVAL, "bad signal number");

    printf("sigtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
