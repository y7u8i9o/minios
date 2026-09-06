/* M41 test: the sampling profiler. A hot function dominating the samples
 * of the process, chains reaching main, filtering by pid, kernel samples
 * resolved through /dev/ksyms, the divider, stop and restart, ring
 * overflow accounting and poll. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <minios/profile.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static volatile unsigned sink;

static double user_seconds(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
}

/* The hot function: spins for secs of this process's user time. */
__attribute__((noinline)) void hot_loop(double secs)
{
    double until = user_seconds() + secs;
    unsigned x = 1;
    for (;;) {
        for (int i = 0; i < 20000; i++)
            x = x * 1664525u + 1013904223u;
        sink = x;
        if (user_seconds() >= until)
            return;
    }
}

__attribute__((noinline)) void outer(double secs)
{
    hot_loop(secs);
    sink++;
}

static struct prof_symtab *self_syms, *kernel_syms;

struct tally { unsigned total, user, kernel, hot, chain_main, other_pid, kernel_named, pid_seen[4]; };

static void tally_samples(int fd, pid_t pid, struct tally *t)
{
    struct prof_sample buf[256];
    ssize_t n;
    while ((n = prof_read(fd, buf, 256)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            struct prof_sample *s = &buf[i];
            t->total++;
            if (pid && (pid_t)s->pid != pid)
                t->other_pid++;
            int kernel = !(s->flags & PROF_FLAG_USER);
            if (kernel) {
                t->kernel++;
                uint64_t off;
                if (prof_symtab_lookup(kernel_syms, s->chain[0], &off))
                    t->kernel_named++;
                continue;
            }
            t->user++;
            const struct prof_sym *sym = prof_symtab_lookup(self_syms, s->chain[0], NULL);
            if (sym && (strcmp(sym->name, "hot_loop") == 0 || strcmp(sym->name, "user_seconds") == 0 ||
                        strcmp(sym->name, "getrusage") == 0 || strcmp(sym->name, "__syscall6") == 0))
                t->hot++;
            for (unsigned d = 1; d < s->depth; d++) {
                const struct prof_sym *c = prof_symtab_lookup(self_syms, s->chain[d], NULL);
                if (c && strcmp(c->name, "main") == 0) {
                    t->chain_main++;
                    break;
                }
            }
        }
    }
}

static void test_symbols(void)
{
    self_syms = prof_symtab_load_elf("/bin/proftest");
    CHECK(self_syms && self_syms->count > 3, "own symbol table: %zu symbols", self_syms ? self_syms->count : 0);
    CHECK(prof_symtab_add_maps(self_syms, getpid()) > 0, "libraries of this process from /dev/maps");
    const struct prof_sym *lib = prof_symtab_lookup(self_syms, (uint64_t)(uintptr_t)qsort + 2, NULL);
    CHECK(lib && strcmp(lib->name, "qsort") == 0, "lookup inside the shared C library: %s", lib ? lib->name : "?");
    uint64_t off = 0;
    const struct prof_sym *s = prof_symtab_lookup(self_syms, (uint64_t)(uintptr_t)hot_loop + 5, &off);
    CHECK(s && strcmp(s->name, "hot_loop") == 0 && off == 5, "lookup inside hot_loop: %s+%lu", s ? s->name : "?", (unsigned long)off);
    CHECK(prof_symtab_lookup(self_syms, 0x1000, NULL) == NULL, "lookup below the text");
    kernel_syms = prof_symtab_load_kernel();
    CHECK(kernel_syms && kernel_syms->count > 100, "kernel symbols from /dev/ksyms: %zu", kernel_syms ? kernel_syms->count : 0);
    int found = 0;
    for (size_t i = 0; kernel_syms && i < kernel_syms->count; i++)
        if (strcmp(kernel_syms->syms[i].name, "sched_tick") == 0 || strcmp(kernel_syms->syms[i].name, "vmm_handle_fault") == 0)
            found++;
    CHECK(found == 2, "well known kernel symbols listed: %d", found);
    char text[80];
    prof_format_addr(self_syms, kernel_syms, 0, (uint64_t)(uintptr_t)outer + 3, 1, text, sizeof text);
    CHECK(strcmp(text, "outer+0x3") == 0, "format with offset: %s", text);
    prof_format_addr(self_syms, kernel_syms, 0, 0x10, 0, text, sizeof text);
    CHECK(strcmp(text, "0x10") == 0, "unknown address stays numeric: %s", text);
}

static void test_self_profile(int fd)
{
    struct prof_stats st;
    CHECK(prof_get_stats(fd, &st) == 0 && !st.enabled && st.ring > 1000, "initial stats: enabled %u ring %u", st.enabled, st.ring);
    CHECK(prof_start(fd, getpid()) == 0, "start: %s", strerror(errno));
    outer(0.5);
    struct pollfd pfd = { fd, POLLIN, 0 };
    CHECK(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN), "poll reports pending samples");
    CHECK(prof_stop(fd) == 0, "stop");
    struct tally t = { 0 };
    tally_samples(fd, getpid(), &t);
    printf("proftest: self: %u samples, %u user, %u kernel, %u hot, %u chains reach main\n",
           t.total, t.user, t.kernel, t.hot, t.chain_main);
    CHECK(t.total >= 200, "enough samples for half a second: %u", t.total);
    CHECK(t.other_pid == 0, "every sample belongs to this pid: %u strays", t.other_pid);
    CHECK(t.user > 0 && t.hot * 100 / (t.user ? t.user : 1) >= 60, "hot loop dominates: %u of %u", t.hot, t.user);
    CHECK(t.chain_main * 100 / (t.user ? t.user : 1) >= 50, "chains reach main: %u of %u", t.chain_main, t.user);
    CHECK(prof_get_stats(fd, &st) == 0 && st.samples == t.total && st.pending == 0, "stats agree: %lu/%u pending %lu",
          (unsigned long)st.samples, t.total, (unsigned long)st.pending);
    /* Nothing arrives after stop. */
    outer(0.1);
    struct prof_sample one;
    CHECK(prof_read(fd, &one, 1) == 0, "no samples after stop");
}

static void test_filter_and_kernel(int fd)
{
    /* A child that spins in user mode and one that makes system calls. */
    pid_t spinner = fork();
    if (spinner == 0) {
        outer(0.6);
        _exit(0);
    }
    pid_t caller = fork();
    if (caller == 0) {
        int null = open("/dev/null", O_WRONLY);
        char buf[4096];
        memset(buf, 0, sizeof buf);
        double until = user_seconds() + 0.3;
        for (;;) {
            for (int i = 0; i < 200; i++)
                write(null, buf, sizeof buf);
            if (user_seconds() >= until)
                break;
        }
        _exit(0);
    }
    CHECK(prof_start(fd, spinner) == 0, "start filtered on the spinner");
    outer(0.3);                             /* the parent spins too but must not appear */
    int status;
    waitpid(spinner, &status, 0);
    prof_stop(fd);
    struct tally t = { 0 };
    tally_samples(fd, spinner, &t);
    printf("proftest: filtered: %u samples, %u strays, %u hot\n", t.total, t.other_pid, t.hot);
    CHECK(t.total >= 100 && t.other_pid == 0, "only the spinner was sampled: %u samples, %u strays", t.total, t.other_pid);
    CHECK(t.hot * 100 / (t.user ? t.user : 1) >= 60, "the child's hot loop dominates: %u of %u", t.hot, t.user);

    CHECK(prof_start(fd, caller) == 0, "start on the caller");
    waitpid(caller, &status, 0);
    prof_stop(fd);
    memset(&t, 0, sizeof t);
    tally_samples(fd, caller, &t);
    printf("proftest: syscall child: %u samples, %u kernel, %u named\n", t.total, t.kernel, t.kernel_named);
    CHECK(t.kernel > 0 && t.kernel_named * 100 / t.kernel >= 90, "kernel samples resolve to symbols: %u of %u", t.kernel_named, t.kernel);
}

static void test_divider_and_overflow(int fd)
{
    CHECK(prof_set_divider(fd, 0) < 0 && errno == EINVAL, "divider 0 refused");
    CHECK(prof_set_divider(fd, 10) == 0, "divider 10");
    prof_start(fd, getpid());
    outer(0.5);
    prof_stop(fd);
    struct tally t = { 0 };
    tally_samples(fd, getpid(), &t);
    printf("proftest: divider 10: %u samples\n", t.total);
    CHECK(t.total >= 20 && t.total <= 120, "about a tenth of the samples: %u", t.total);
    prof_set_divider(fd, 1);

    /* Every process, unread for long enough to fill the ring. */
    struct prof_stats st;
    prof_get_stats(fd, &st);
    unsigned ring = st.ring;
    pid_t kids[3];
    for (int i = 0; i < 3; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            outer(2.5);
            _exit(0);
        }
    }
    prof_start(fd, 0);
    outer(2.5);
    for (int i = 0; i < 3; i++) {
        int status;
        waitpid(kids[i], &status, 0);
    }
    prof_stop(fd);
    prof_get_stats(fd, &st);
    printf("proftest: overflow: %lu samples, %lu dropped, %lu pending of %u\n", (unsigned long)st.samples,
           (unsigned long)st.dropped, (unsigned long)st.pending, ring);
    CHECK(st.dropped > 0 && st.pending == ring, "ring filled and overflow counted");
    memset(&t, 0, sizeof t);
    tally_samples(fd, 0, &t);
    CHECK(t.total == ring, "read back the whole ring: %u", t.total);
    prof_get_stats(fd, &st);
    CHECK(st.pending == 0, "ring drained");
}

int main(void)
{
    printf("proftest: pid %d\n", getpid());
    int fd = prof_open();
    CHECK(fd >= 0, "open /dev/profile: %s", strerror(errno));
    if (fd < 0)
        return 1;
    test_symbols();
    test_self_profile(fd);
    test_filter_and_kernel(fd);
    test_divider_and_overflow(fd);
    close(fd);
    printf("proftest: %d failures\n", failures);
    return failures ? 1 : 0;
}
