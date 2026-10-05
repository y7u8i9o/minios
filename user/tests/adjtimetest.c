/* The system call adjtime (boot case adjtime, V2 of
 * docs/plan/release-0.6.0.md). The program runs as root. It checks the
 * rate of a slew of 100 ms against the monotonic clock, the remaining
 * correction that adjtime reports, the replacement of a slew by a new
 * call, the end of a slew by clock_settime, the refusal of an invalid
 * value, and EPERM for a user other than root. Exits 0 when every check
 * passes. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("adjtimetest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static long long now_us(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static long long remaining_us(void)
{
    struct timeval old = { 99, 99 };
    if (adjtime(NULL, &old) != 0)
        return -999999999;
    return (long long)old.tv_sec * 1000000 + old.tv_usec;
}

int main(void)
{
    CHECK(remaining_us() == 0, "a slew before the first call: %lld us", remaining_us());

    /* 100 ms forward at 500 us per second: after one second the realtime
     * clock is 500 us ahead of the monotonic clock. */
    struct timeval delta = { 0, 100000 };
    CHECK(adjtime(&delta, NULL) == 0, "adjtime +100 ms: %d", errno);
    long long mono0 = now_us(CLOCK_MONOTONIC), real0 = now_us(CLOCK_REALTIME);
    usleep(1000000);
    long long mono1 = now_us(CLOCK_MONOTONIC), real1 = now_us(CLOCK_REALTIME);
    long long gained = (real1 - real0) - (mono1 - mono0), expected = (mono1 - mono0) / 2000;
    CHECK(gained >= expected - 30 && gained <= expected + 30, "the slew gained %lld us in %lld us, expected %lld",
          gained, mono1 - mono0, expected);
    long long left = remaining_us();
    CHECK(left > 99000 && left < 99600, "remaining after one second: %lld us", left);
    printf("adjtimetest: slew of 100 ms gained %lld us in %lld us, %lld us remain\n", gained, mono1 - mono0, left);

    /* A new call replaces the slew and reports what remained. */
    struct timeval back = { 0, -20000 }, old;
    CHECK(adjtime(&back, &old) == 0 && old.tv_sec == 0 && old.tv_usec > 99000, "replacement: old %lld.%06ld",
          (long long)old.tv_sec, (long)old.tv_usec);
    left = remaining_us();
    CHECK(left <= -19900 && left >= -20000, "the new slew: %lld us", left);

    /* A step of the clock ends the slew. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    CHECK(clock_settime(CLOCK_REALTIME, &ts) == 0, "clock_settime");
    CHECK(remaining_us() == 0, "a step ends the slew: %lld us", remaining_us());

    struct timeval bad = { 0, 1000000 };
    errno = 0;
    CHECK(adjtime(&bad, NULL) == -1 && errno == EINVAL, "an invalid microsecond value: errno %d", errno);

    /* Another user may read the slew but not change it. */
    pid_t pid = fork();
    if (pid == 0) {
        if (setuid(1000) != 0)
            _exit(3);
        struct timeval d = { 0, 1000 };
        errno = 0;
        int r = adjtime(&d, NULL);
        if (r != -1 || errno != EPERM)
            _exit(1);
        _exit(adjtime(NULL, &d) == 0 ? 0 : 2);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a user other than root: status 0x%x", status);

    if (failures) {
        printf("adjtimetest: %d failures\n", failures);
        return 1;
    }
    printf("adjtimetest: ok\n");
    return 0;
}
