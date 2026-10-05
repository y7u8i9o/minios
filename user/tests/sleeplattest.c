/* Timed sleeps of user programs (boot case sleep_latency).
 *
 * The first part measures the wake-up latency of a timed sleep while every
 * CPU runs a busy process.  The program starts one process per CPU that
 * computes without a pause, then sleeps 10 ms fifty times and measures
 * each sleep with CLOCK_MONOTONIC.  The overshoot of a sleep is the
 * measured time minus 10 ms.  The program prints the mean and the maximum
 * overshoot.  The case requires a maximum below 15 ms, a little more than
 * the 10 ms time slice of the highest level of the scheduler.
 *
 * The second part checks the signals.  A handled signal ends nanosleep with
 * EINTR and the remaining time, an ignored signal does not shorten a sleep,
 * and sleep returns the unslept seconds.  Exits 0 when every check passes. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SLEEPS 50
#define SLEEP_MS 10
#define LIMIT_MS 15
#define MAX_BUSY 16

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("sleeplattest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static void on_usr1(int sig)
{
    (void)sig;
}

/* send_later starts a child that sends sig to this process after ms. */
static pid_t send_later(int sig, int ms)
{
    pid_t parent = getpid(), child = fork();
    if (child == 0) {
        usleep((unsigned long)ms * 1000);
        kill(parent, sig);
        _exit(0);
    }
    return child;
}

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void check_signals(void)
{
    /* A handled signal after 50 ms ends a 500 ms sleep. */
    signal(SIGUSR1, on_usr1);
    pid_t child = send_later(SIGUSR1, 50);
    struct timespec request = { 0, 500000000 }, remain = { 0, 0 };
    long long start = now_us();
    int r = nanosleep(&request, &remain);
    int err = errno;
    long long slept = now_us() - start;
    waitpid(child, NULL, 0);
    long long left_ms = (long long)remain.tv_sec * 1000 + remain.tv_nsec / 1000000;
    CHECK(r == -1 && err == EINTR, "nanosleep after a handled signal: %d errno %d", r, err);
    CHECK(slept < 400000, "the handled signal did not end the sleep: %lld us", slept);
    CHECK(left_ms > 0 && left_ms <= 500 - slept / 1000 + 2, "remaining time %lld ms after %lld us", left_ms, slept);
    printf("sleeplattest: handled signal ended a 500 ms sleep after %lld us, %lld ms remaining\n", slept, left_ms);

    /* An ignored signal does not shorten a sleep. */
    signal(SIGUSR2, SIG_IGN);
    child = send_later(SIGUSR2, 20);
    start = now_us();
    r = usleep(200000);
    slept = now_us() - start;
    waitpid(child, NULL, 0);
    CHECK(r == 0 && slept >= 200000, "an ignored signal shortened a 200 ms sleep to %lld us", slept);
    printf("sleeplattest: ignored signal, 200 ms sleep lasted %lld us\n", slept);

    /* sleep returns the unslept seconds. */
    child = send_later(SIGUSR1, 100);
    unsigned left = sleep(3);
    waitpid(child, NULL, 0);
    CHECK(left == 3, "sleep(3) after a signal at 100 ms returned %u", left);
    printf("sleeplattest: sleep(3) interrupted after 100 ms returned %u\n", left);
}

int main(void)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1)
        cpus = 1;
    if (cpus > MAX_BUSY)
        cpus = MAX_BUSY;
    pid_t busy[MAX_BUSY];
    for (long i = 0; i < cpus; i++) {
        busy[i] = fork();
        if (busy[i] == 0) {
            for (;;)
                __asm__ volatile("" ::: "memory");
        }
    }
    /* The busy processes run for a second first.  The scheduler demotes
     * them to its lower levels, which have the longer time slices. */
    sleep(1);
    long long total = 0, worst = 0;
    for (int i = 0; i < SLEEPS; i++) {
        long long start = now_us();
        usleep(SLEEP_MS * 1000);
        long long over = now_us() - start - SLEEP_MS * 1000;
        total += over;
        if (over > worst)
            worst = over;
    }
    for (long i = 0; i < cpus; i++) {
        kill(busy[i], SIGKILL);
        waitpid(busy[i], NULL, 0);
    }
    printf("sleeplattest: %ld busy processes, overshoot mean %lld us, max %lld us\n", cpus, total / SLEEPS, worst);
    CHECK(worst < LIMIT_MS * 1000, "the maximum overshoot reaches %d ms", LIMIT_MS);
    check_signals();
    if (failures)
        return 1;
    printf("sleeplattest: ok\n");
    return 0;
}
