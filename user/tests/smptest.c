/* M18 test: several processes run at the same time on different CPUs.
 * Each child spins for a while, samples getcpu() and reports the set of
 * CPUs it ran on through a pipe. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define SPIN_MS 400

int main(void)
{
    int ncpu = nproc();
    printf("smptest: %d cpus, running on cpu %d\n", ncpu, getcpu());
    CHECK(ncpu >= 2, "nproc %d", ncpu);
    CHECK(getcpu() >= 0 && getcpu() < ncpu, "getcpu");

    int nchildren = ncpu * 2;
    int fds[2];
    CHECK(pipe(fds) == 0, "pipe");
    long start = uptime_ms();
    for (int i = 0; i < nchildren; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            unsigned mask = 0;
            volatile unsigned long work = 0;
            long until = uptime_ms() + SPIN_MS;
            while (uptime_ms() < until) {
                for (int k = 0; k < 10000; k++)
                    work++;
                mask |= 1u << getcpu();
                if (work == 0)
                    mask = 0;
            }
            write(fds[1], &mask, sizeof mask);
            _exit(0);
        }
        CHECK(pid > 0, "fork");
    }
    unsigned all = 0;
    int reports = 0;
    for (int i = 0; i < nchildren; i++) {
        unsigned mask;
        if (read(fds[0], &mask, sizeof mask) == (long)sizeof mask) {
            all |= mask;
            reports++;
        }
    }
    for (int i = 0; i < nchildren; i++) {
        int status;
        CHECK(wait(&status) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child status");
    }
    long elapsed = uptime_ms() - start;
    int used = 0;
    for (int i = 0; i < ncpu; i++)
        used += (all >> i) & 1;
    printf("smptest: %d children reported, cpus used %d of %d, %ld ms for %d ms of work each\n",
           reports, used, ncpu, elapsed, SPIN_MS);
    CHECK(reports == nchildren, "reports %d", reports);
    CHECK(used >= 2, "children ran on %d cpu(s)", used);
    /* With every child spinning for SPIN_MS, sequential execution would
     * take nchildren * SPIN_MS; parallel execution takes clearly less. */
    CHECK(elapsed < (long)nchildren * SPIN_MS, "no parallel speedup: %ld ms", elapsed);
    printf("smptest: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
