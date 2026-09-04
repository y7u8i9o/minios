/* time: run a command and report its real, user and system time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/resource.h>

static double seconds(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: time command [args...]\n");
        return 2;
    }
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "time: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execvp(argv[1], argv + 1);
        fprintf(stderr, "time: %s: %s\n", argv[1], strerror(errno));
        _exit(127);
    }
    int status = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof ru);
    wait4(pid, &status, 0, &ru);
    clock_gettime(CLOCK_MONOTONIC, &end);
    fprintf(stderr, "\nreal %.3fs\nuser %.3fs\nsys  %.3fs\n", seconds(&start, &end),
            (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6,
            (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6);
    if (ru.ru_maxrss)
        fprintf(stderr, "rss  %ld KiB, %ld faults, %ld/%ld switches\n", (long)ru.ru_maxrss,
                (long)(ru.ru_minflt + ru.ru_majflt), (long)ru.ru_nvcsw, (long)ru.ru_nivcsw);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
