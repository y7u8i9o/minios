/* M40 test: resource limits and CPU accounting. Exits 0 on success; with
 * "--stack" it is the exec'd child that probes its own stack size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define PG 4096UL

static int in_child(void (*fn)(void *), void *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        fn(arg);
        _exit(0);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

static void touch_read(void *p) { volatile unsigned char *c = p; (void)*c; }

/* The exec'd child: argv[2] holds the expected stack size in KiB. A
 * probe well inside the stack works, one beyond it faults. */
static int stack_probe(const char *kb_text)
{
    unsigned long size = strtoul(kb_text, NULL, 10) * 1024;
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) < 0 || rl.rlim_cur != size) {
        printf("stack probe: inherited limit %lu, want %lu\n", (unsigned long)rl.rlim_cur, size);
        return 1;
    }
    volatile unsigned char here = 0;
    unsigned char *sp = (unsigned char *)&here;
    unsigned char *inside = sp - (size - 64 * 1024);
    unsigned char *outside = sp - (size + 64 * 1024);
    int status = in_child(touch_read, inside);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("stack probe: access %lu bytes below the top faulted (status %x)\n", size - 64 * 1024, status);
        return 2;
    }
    status = in_child(touch_read, outside);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        printf("stack probe: access beyond the stack did not fault (status %x)\n", status);
        return 3;
    }
    return 0;
}

static void test_get_set(void)
{
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == OPEN_MAX && rl.rlim_max == OPEN_MAX,
          "default NOFILE %lu/%lu", (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);
    CHECK(getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur == (8UL << 20) && rl.rlim_max == RLIM_INFINITY,
          "default STACK");
    CHECK(getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur == RLIM_INFINITY, "default AS unlimited");
    CHECK(getrlimit(RLIMIT_NLIMITS, &rl) < 0 && errno == EINVAL, "bad resource");
    CHECK(getrlimit(RLIMIT_CPU, (void *)1) < 0 && errno == EFAULT, "bad pointer");
    rl.rlim_cur = 10;
    rl.rlim_max = 5;
    CHECK(setrlimit(RLIMIT_CORE, &rl) < 0 && errno == EINVAL, "soft above hard refused");
    rl.rlim_cur = 5;
    rl.rlim_max = 10;
    CHECK(setrlimit(RLIMIT_CORE, &rl) == 0, "set CORE");
    CHECK(getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur == 5 && rl.rlim_max == 10, "CORE read back");
    rl.rlim_cur = rl.rlim_max = OPEN_MAX + 1;
    CHECK(setrlimit(RLIMIT_NOFILE, &rl) < 0 && errno == EPERM, "NOFILE above OPEN_MAX refused");
    /* Inheritance across fork and exec, prlimit on another pid. */
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit c;
        /* Inherited 5/10; the parent may already have changed it to 7. */
        if (getrlimit(RLIMIT_CORE, &c) < 0 || (c.rlim_cur != 5 && c.rlim_cur != 7) || c.rlim_max != 10)
            _exit(1);
        /* Wait for the parent to change our limit from outside. */
        for (int i = 0; i < 200; i++) {
            getrlimit(RLIMIT_CORE, &c);
            if (c.rlim_cur == 7)
                _exit(0);
            usleep(10000);
        }
        _exit(2);
    }
    struct rlimit n = { 7, 10 }, old;
    CHECK(prlimit(pid, RLIMIT_CORE, &n, &old) == 0 && old.rlim_cur == 5, "prlimit on the child: %s", strerror(errno));
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child inherited and saw the change: %x", status);
    CHECK(prlimit(99999, RLIMIT_CORE, NULL, &old) < 0 && errno == ESRCH, "prlimit on a missing pid");
    CHECK(prlimit(0, RLIMIT_CORE, NULL, &old) == 0 && old.rlim_cur == 5, "prlimit pid 0 is the caller");
}

static void test_memory_limits(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* RLIMIT_AS: a little headroom, then a big mapping fails. */
        void *probe = mmap(NULL, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        munmap(probe, PG);
        void *base = sbrk(0);
        struct rlimit rl;
        getrlimit(RLIMIT_AS, &rl);
        /* Find the current total: grow the limit down until mmap of one
         * page fails, using the limit as the measuring stick. */
        unsigned long total = 0;
        for (unsigned long lim = 1UL << 20; lim < (1UL << 32); lim += 1UL << 20) {
            rl.rlim_cur = lim;
            setrlimit(RLIMIT_AS, &rl);
            void *p = mmap(NULL, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) {
                munmap(p, PG);
                total = lim;
                break;
            }
        }
        if (!total)
            _exit(1);
        rl.rlim_cur = total + (1UL << 20);
        setrlimit(RLIMIT_AS, &rl);
        void *big = mmap(NULL, 4UL << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (big != MAP_FAILED || errno != ENOMEM)
            _exit(2);
        void *small = mmap(NULL, 256 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (small == MAP_FAILED)
            _exit(3);
        if (sbrk(2 << 20) != (void *)-1 || errno != ENOMEM)
            _exit(4);
        rl.rlim_cur = RLIM_INFINITY;
        setrlimit(RLIMIT_AS, &rl);
        /* RLIMIT_DATA bounds the heap. */
        void *brk = sbrk(0);
        unsigned long heap = (unsigned long)brk - (unsigned long)base + PG;   /* base page plus growth */
        rl.rlim_cur = heap + 64 * 1024;
        rl.rlim_max = RLIM_INFINITY;
        setrlimit(RLIMIT_DATA, &rl);
        if (sbrk(1 << 20) != (void *)-1 || errno != ENOMEM)
            _exit(5);
        if (sbrk(16 * 1024) == (void *)-1)
            _exit(6);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "AS and DATA limits: status %x", status);
}

static void test_nofile_nproc(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit rl = { 5, OPEN_MAX };
        if (setrlimit(RLIMIT_NOFILE, &rl) < 0)
            _exit(1);
        int a = open("/dev/null", O_RDONLY);       /* 3 */
        int b = open("/dev/null", O_RDONLY);       /* 4 */
        if (a != 3 || b != 4)
            _exit(2);
        if (open("/dev/null", O_RDONLY) >= 0 || errno != EMFILE)
            _exit(3);
        if (dup(a) >= 0 || errno != EMFILE)
            _exit(4);
        if (dup2(a, 6) >= 0 || errno != EBADF)
            _exit(5);
        close(b);
        if (dup(a) != 4)
            _exit(6);
        rl.rlim_cur = 1;                            /* already open descriptors stay usable */
        setrlimit(RLIMIT_NOFILE, &rl);
        char c;
        if (read(a, &c, 1) != 0)
            _exit(7);
        /* RLIMIT_NPROC: no more processes than the limit. */
        struct rlimit np = { 1, RLIM_INFINITY };
        setrlimit(RLIMIT_NPROC, &np);
        pid_t k = fork();
        if (k >= 0) {
            if (k == 0)
                _exit(0);
            _exit(8);
        }
        if (errno != EAGAIN)
            _exit(9);
        np.rlim_cur = RLIM_INFINITY;
        setrlimit(RLIMIT_NPROC, &np);
        k = fork();
        if (k == 0)
            _exit(0);
        if (k < 0)
            _exit(10);
        int st;
        waitpid(k, &st, 0);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "NOFILE and NPROC: status %x", status);
}

static volatile int xfsz_seen;
static void on_xfsz(int sig) { xfsz_seen++; }

static void test_fsize(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGXFSZ, on_xfsz);
        struct rlimit rl = { 10000, RLIM_INFINITY };
        if (setrlimit(RLIMIT_FSIZE, &rl) < 0)
            _exit(1);
        int fd = open("/rlimit.dat", O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (fd < 0)
            _exit(2);
        char buf[8192];
        memset(buf, 'x', sizeof buf);
        if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf)
            _exit(3);
        ssize_t n = write(fd, buf, sizeof buf);      /* clamped to the limit */
        if (n != 10000 - 8192)
            _exit(4);
        if (write(fd, buf, 1) >= 0 || errno != EFBIG)
            _exit(5);
        if (xfsz_seen != 1)
            _exit(6);
        /* Devices are not files: writing the console still works. */
        if (write(1, "", 0) < 0)
            _exit(7);
        close(fd);
        unlink("/rlimit.dat");
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "FSIZE: status %x", status);
    /* Without a handler the default action terminates. */
    pid = fork();
    if (pid == 0) {
        struct rlimit rl = { 100, RLIM_INFINITY };
        setrlimit(RLIMIT_FSIZE, &rl);
        int fd = open("/rlimit2.dat", O_CREAT | O_TRUNC | O_RDWR, 0644);
        char buf[200];
        memset(buf, 'y', sizeof buf);
        write(fd, buf, sizeof buf);
        write(fd, buf, sizeof buf);                  /* SIGXFSZ terminates here */
        _exit(1);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGXFSZ, "SIGXFSZ default terminates: %x", status);
    unlink("/rlimit2.dat");
}

static void test_stack(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit rl = { 256 * 1024, RLIM_INFINITY };
        if (setrlimit(RLIMIT_STACK, &rl) < 0)
            _exit(1);
        execv("/bin/rlimittest", (char *const[]){ "rlimittest", "--stack", "256", NULL });
        _exit(100);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "stack size follows RLIMIT_STACK: status %x", status);
    /* Too small a limit is raised to the minimum. */
    pid = fork();
    if (pid == 0) {
        struct rlimit rl = { 4096, RLIM_INFINITY };
        setrlimit(RLIMIT_STACK, &rl);
        execv("/bin/rlimittest", (char *const[]){ "rlimittest", "--stack-min", NULL });
        _exit(100);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "tiny stack limit still runs: status %x", status);
}

static volatile int xcpu_seen;
static void on_xcpu(int sig) { xcpu_seen++; }

static void spin_seconds(double secs)
{
    struct rusage ru;
    for (;;) {
        for (volatile int i = 0; i < 100000; i++)
            ;
        getrusage(RUSAGE_SELF, &ru);
        if ((double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 >= secs)
            return;
    }
}

static void test_cpu(void)
{
    /* Soft limit without a handler: SIGXCPU terminates. */
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit rl = { 1, 5 };
        setrlimit(RLIMIT_CPU, &rl);
        spin_seconds(4);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGXCPU, "SIGXCPU at the soft limit: %x", status);
    /* With a handler the process runs on until the hard limit kills it. */
    pid = fork();
    if (pid == 0) {
        signal(SIGXCPU, on_xcpu);
        struct rlimit rl = { 1, 2 };
        setrlimit(RLIMIT_CPU, &rl);
        spin_seconds(4);
        _exit(xcpu_seen ? 1 : 2);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "SIGKILL at the hard limit: %x", status);
}

static void test_rusage(void)
{
    struct rusage before, after, child;
    CHECK(getrusage(RUSAGE_SELF, &before) == 0, "getrusage self");
    CHECK(getrusage(99, &after) < 0 && errno == EINVAL, "getrusage bad who");
    /* Burn some user time and touch pages. */
    unsigned char *p = mmap(NULL, 64 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (size_t i = 0; i < 64 * PG; i += PG)
        p[i] = 1;
    spin_seconds((double)before.ru_utime.tv_sec + (double)before.ru_utime.tv_usec / 1e6 + 0.3);
    CHECK(getrusage(RUSAGE_SELF, &after) == 0, "getrusage after");
    double du = ((double)after.ru_utime.tv_sec + (double)after.ru_utime.tv_usec / 1e6) -
                ((double)before.ru_utime.tv_sec + (double)before.ru_utime.tv_usec / 1e6);
    CHECK(du >= 0.25 && du < 2.0, "user time grew by %.3f s", du);
    CHECK(after.ru_minflt >= before.ru_minflt + 64, "minor faults counted: %ld -> %ld", (long)before.ru_minflt, (long)after.ru_minflt);
    CHECK(after.ru_maxrss >= 64 * 4, "resident size %ld KiB", (long)after.ru_maxrss);
    munmap(p, 64 * PG);
    /* Children's time is accumulated when they are reaped. */
    CHECK(getrusage(RUSAGE_CHILDREN, &before) == 0, "getrusage children");
    pid_t pid = fork();
    if (pid == 0) {
        spin_seconds(0.5);
        _exit(0);
    }
    int status;
    struct rusage w;
    CHECK(wait4(pid, &status, 0, &w) == pid, "wait4 with rusage");
    double wu = (double)w.ru_utime.tv_sec + (double)w.ru_utime.tv_usec / 1e6;
    CHECK(wu >= 0.45 && wu < 3.0, "wait4 reports the child's user time %.3f", wu);
    CHECK(getrusage(RUSAGE_CHILDREN, &child) == 0, "children after reap");
    double dc = ((double)child.ru_utime.tv_sec + (double)child.ru_utime.tv_usec / 1e6) -
                ((double)before.ru_utime.tv_sec + (double)before.ru_utime.tv_usec / 1e6);
    CHECK(dc >= 0.45, "children total grew by %.3f", dc);
    struct rusage th;
    CHECK(getrusage(RUSAGE_THREAD, &th) == 0 && th.ru_utime.tv_sec + th.ru_utime.tv_usec > 0, "thread usage");
    CHECK(after.ru_nvcsw + after.ru_nivcsw > 0, "context switches counted");
    /* /dev/proc carries TIME and RSS columns. */
    int fd = open("/dev/proc", O_RDONLY);
    char text[2048];
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    text[n > 0 ? n : 0] = '\0';
    CHECK(strstr(text, "TIME") && strstr(text, "RSS"), "/dev/proc header: %.60s", text);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--stack") == 0)
        return stack_probe(argv[2]);
    if (argc >= 2 && strcmp(argv[1], "--stack-min") == 0) {
        char big[40000];                    /* needs more than the 4 KiB asked for */
        memset(big, 1, sizeof big);
        return big[0] == 1 ? 0 : 1;
    }
    printf("rlimittest: pid %d\n", getpid());
    test_get_set();
    test_memory_limits();
    test_nofile_nproc();
    test_fsize();
    test_stack();
    test_cpu();
    test_rusage();
    printf("rlimittest: %d failures\n", failures);
    return failures ? 1 : 0;
}
