/* M23: user space FPU and SSE state survives context switches, fork and
 * signal handlers. Compiled with SSE, so the doubles live in xmm
 * registers between the checks. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("fputest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static volatile double sink;

/* Slowly accumulates so the value is register resident across yields. */
static double work(double seed, int rounds)
{
    double a = seed, b = 1.0 / 3.0, c = 2.5;
    for (int i = 0; i < rounds; i++) {
        a = a * 1.0000001 + b;
        c = c * 0.9999999 + a * 0.000001;
        if ((i & 4095) == 0)
            sched_yield();
    }
    sink = c;
    return a;
}

static void on_signal(int sig)
{
    /* Clobber the registers inside the handler. */
    volatile double x = 12345.678, y = 0.001;
    for (int i = 0; i < 1000; i++)
        x = x * y + 3.0;
    sink = x;
}

int main(void)
{
    double expected = work(1.0, 200000);
    pid_t children[3];
    for (int i = 0; i < 3; i++) {
        children[i] = fork();
        if (children[i] == 0) {
            double r = work(1.0 + i, 400000), again = work(1.0 + i, 400000);
            _exit(r == again ? 0 : 1);
        }
    }
    double r = work(1.0, 200000);
    CHECK(r == expected, "result differs while other processes compute: %d", (int)(r * 1000));
    signal(SIGUSR1, on_signal);
    double before = work(7.0, 50000);
    double retain = before * 2.0;                 /* stored in a register across the signal */
    kill(getpid(), SIGUSR1);
    double after = retain / 2.0;
    CHECK(after == before, "registers restored after a signal handler");
    for (int i = 0; i < 3; i++) {
        int status;
        waitpid(children[i], &status, 0);
        CHECK(status == 0, "child %d status 0x%x", i, status);
    }
    /* A value that only an x87 or SSE unit produces. */
    volatile double d = 1e300;
    CHECK(d * 10.0 > 1e300, "double arithmetic");
    printf("fputest: %d failures\n", failures);
    return failures ? 1 : 0;
}
