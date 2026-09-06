/* init: process 1. Mounts the filesystems of /etc/fstab through fsinit,
 * starts the shell and restarts it when it exits, reaps orphaned processes,
 * and performs the orderly shutdown when asked with SIGUSR1 (power off),
 * SIGUSR2 (reboot) or SIGHUP (halt). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/reboot.h>

static volatile int shutdown_request;   /* the signal received, 0 if none */

static void on_signal(int sig)
{
    shutdown_request = sig;
}

int main(int argc, char **argv)
{
    printf("init: pid %d\n", getpid());
    /* Default actions never terminate init, so SIGINT and SIGTERM need no
     * handling here; ignoring them would be inherited by the shell. */
    signal(SIGUSR1, on_signal);
    signal(SIGUSR2, on_signal);
    signal(SIGHUP, on_signal);
    /* The persistent volumes are mounted before the shell starts; a
     * failure is reported and the system continues from the root image. */
    pid_t fs = fork();
    if (fs == 0) {
        char *const args[] = { "fsinit", NULL };
        execv("/bin/fsinit", args);
        perror("init: exec fsinit");
        _exit(127);
    }
    int fs_status = 0;
    if (fs > 0 && waitpid(fs, &fs_status, 0) == fs && (!WIFEXITED(fs_status) || WEXITSTATUS(fs_status) != 0))
        printf("init: fsinit failed with status %d\n", WIFEXITED(fs_status) ? WEXITSTATUS(fs_status) : -1);
    pid_t shell = -1;
    for (;;) {
        if (shutdown_request) {
            int cmd = shutdown_request == SIGUSR2 ? RB_AUTOBOOT
                    : shutdown_request == SIGHUP ? RB_HALT : RB_POWER_OFF;
            printf("init: %s\n", cmd == RB_AUTOBOOT ? "rebooting" : cmd == RB_HALT ? "halting" : "powering off");
            fflush(stdout);
            reboot(cmd);
            perror("init: reboot");
            shutdown_request = 0;
        }
        if (shell < 0) {
            shell = fork();
            if (shell == 0) {
                setenv("PATH", "/bin", 1);
                setenv("HOME", "/home", 1);
                setenv("USER", "user", 1);
                setenv("SHELL", "/bin/sh", 1);
                setenv("TERM", "minios", 1);
                setpgid(0, 0);
                tcsetpgrp(0, getpid());
                char *const args[] = { "sh", NULL };
                execvp("sh", args);
                perror("init: exec sh");
                _exit(127);
            }
            if (shell < 0) {
                perror("init: fork");
                return 1;
            }
            setpgid(shell, shell);
        }
        int status;
        pid_t pid = wait(&status);
        if (pid < 0)
            continue;   /* EINTR from a signal */
        if (pid == shell) {
            printf("init: shell exited with status %d, restarting\n", WEXITSTATUS(status));
            shell = -1;
        }
    }
}
