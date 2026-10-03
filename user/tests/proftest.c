/* M48 test: the system profiler. Symbol tables, CPU samples with deep and
 * recursive chains, stitching of kernel chains onto the user frames that
 * caused them, the pid filter, the divider, ring overflow, the scheduler,
 * kernel heap and transfer classes, and the aggregation the front ends
 * use: call trees, flame graph layout and folded stacks. Exits 0 on
 * success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <prof/profile.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define READ_BYTES 65536
static char *buffer;
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

/* A recursive chain, so the samples carry more frames than the eight the
 * first profiler could hold. */
__attribute__((noinline)) void deep(int n, double secs)
{
    if (n > 0) {
        deep(n - 1, secs);
        sink++;
        return;
    }
    outer(secs);
}

static struct prof_symtab *self_syms, *kernel_syms;

struct tally {
    unsigned total, user, kernel, hot, chain_main, other_pid, kernel_named;
    unsigned stitched, deepest, counts[PROF_EV_TYPES];
    uint64_t off_cpu_ns, io_bytes, alloc_bytes;
};

static void tally(int fd, pid_t pid, struct tally *t)
{
    ssize_t n;
    while ((n = prof_read_events(fd, buffer, READ_BYTES)) > 0) {
        for (const struct prof_event *e = prof_event_first(buffer, (size_t)n); e;
             e = prof_event_next(buffer, (size_t)n, e)) {
            t->total++;
            if (e->type < PROF_EV_TYPES)
                t->counts[e->type]++;
            if (pid && (pid_t)e->pid != pid)
                t->other_pid++;
            if (e->type == PROF_EV_RUN)
                t->off_cpu_ns += e->a;
            if (e->type == PROF_EV_IO)
                t->io_bytes += e->a;
            if (e->type == PROF_EV_ALLOC)
                t->alloc_bytes += e->b;
            if (e->type != PROF_EV_SAMPLE)
                continue;
            if (e->depth > t->deepest)
                t->deepest = e->depth;
            if (e->flags & PROF_FLAG_KUSER)
                t->stitched++;
            int kernel = !(e->flags & PROF_FLAG_USER);
            if (kernel) {
                t->kernel++;
                if (prof_symtab_lookup(kernel_syms, e->chain[0], NULL))
                    t->kernel_named++;
            } else {
                t->user++;
                const struct prof_sym *sym = prof_symtab_lookup(self_syms, e->chain[0], NULL);
                if (sym && (strcmp(sym->name, "hot_loop") == 0 || strcmp(sym->name, "user_seconds") == 0 ||
                            strcmp(sym->name, "getrusage") == 0 || strcmp(sym->name, "__syscall6") == 0))
                    t->hot++;
            }
            /* main appears in the user part of a chain whether the sample
             * interrupted user code or was stitched onto it. */
            int in_kernel = kernel;
            for (unsigned d = 1; d < e->depth; d++) {
                if (e->chain[d] == PROF_FRAME_BOUNDARY) {
                    in_kernel = 0;
                    continue;
                }
                if (in_kernel)
                    continue;
                const struct prof_sym *c = prof_symtab_lookup(self_syms, e->chain[d], NULL);
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
    CHECK(prof_is_lock_primitive("pop_cli") && !prof_is_lock_primitive("main"), "lock primitives named");
}

static void test_self_profile(int fd)
{
    struct prof_stats st;
    CHECK(prof_get_stats(fd, &st) == 0 && !st.enabled && st.ring > 4096, "initial stats: enabled %u ring %u", st.enabled, st.ring);
    CHECK(prof_start(fd, getpid()) == 0, "start: %s", strerror(errno));
    CHECK(prof_get_stats(fd, &st) == 0 && st.period_ns == 1000000, "one sample stands for a millisecond: %u", st.period_ns);
    deep(12, 0.5);
    struct pollfd pfd = { fd, POLLIN, 0 };
    CHECK(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN), "poll reports pending events");
    CHECK(prof_stop(fd) == 0, "stop");
    struct tally t = { 0 };
    tally(fd, getpid(), &t);
    printf("proftest: self: %u samples, %u user, %u kernel, %u hot, %u reach main, deepest chain %u\n",
           t.total, t.user, t.kernel, t.hot, t.chain_main, t.deepest);
    CHECK(t.total >= 200, "enough samples for half a second: %u", t.total);
    CHECK(t.other_pid == 0, "every event belongs to this pid: %u strays", t.other_pid);
    CHECK(t.user > 0 && t.hot * 100 / (t.user ? t.user : 1) >= 60, "hot loop dominates: %u of %u", t.hot, t.user);
    CHECK(t.chain_main * 100 / (t.user ? t.user : 1) >= 50, "chains reach main: %u of %u", t.chain_main, t.user);
    CHECK(t.deepest > 8, "recursion produces chains deeper than the old limit: %u", t.deepest);
    CHECK(prof_get_stats(fd, &st) == 0 && st.counts[PROF_EV_SAMPLE] == t.total && st.pending == 0,
          "stats agree: %llu/%u pending %llu", (unsigned long long)st.counts[PROF_EV_SAMPLE], t.total,
          (unsigned long long)st.pending);
    outer(0.1);
    CHECK(prof_read_events(fd, buffer, READ_BYTES) == 0, "nothing arrives after stop");
}

static void test_filter_and_kernel(int fd)
{
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
    tally(fd, spinner, &t);
    printf("proftest: filtered: %u samples, %u strays, %u hot\n", t.total, t.other_pid, t.hot);
    CHECK(t.total >= 100 && t.other_pid == 0, "only the spinner was sampled: %u samples, %u strays", t.total, t.other_pid);
    CHECK(t.hot * 100 / (t.user ? t.user : 1) >= 60, "the child's hot loop dominates: %u of %u", t.hot, t.user);

    CHECK(prof_start(fd, caller) == 0, "start on the caller");
    waitpid(caller, &status, 0);
    prof_stop(fd);
    memset(&t, 0, sizeof t);
    tally(fd, caller, &t);
    printf("proftest: syscall child: %u samples, %u kernel, %u named, %u stitched onto user frames\n",
           t.total, t.kernel, t.kernel_named, t.stitched);
    CHECK(t.kernel > 0 && t.kernel_named * 100 / (t.kernel ? t.kernel : 1) >= 90,
          "kernel samples resolve to symbols: %u of %u", t.kernel_named, t.kernel);
    CHECK(t.stitched * 100 / (t.kernel ? t.kernel : 1) >= 50,
          "kernel chains continue into user frames: %u of %u", t.stitched, t.kernel);
}

/* A child that blocks repeatedly, so its off CPU time is known in advance. */
static void test_scheduler(int fd)
{
    struct prof_config cfg = { .events = PROF_MASK_CPU | PROF_MASK_SCHED, .max_depth = PROF_MAX_FRAMES };
    CHECK(prof_configure(fd, &cfg) == 0, "record the scheduler class");
    pid_t sleeper = fork();
    if (sleeper == 0) {
        for (int i = 0; i < 10; i++)
            usleep(20000);
        _exit(0);
    }
    CHECK(prof_start(fd, sleeper) == 0, "start on the sleeper");
    int status;
    waitpid(sleeper, &status, 0);
    prof_stop(fd);
    struct tally t = { 0 };
    tally(fd, sleeper, &t);
    printf("proftest: sleeper: %u blocks, %u runs, %llu ms off CPU\n", t.counts[PROF_EV_BLOCK],
           t.counts[PROF_EV_RUN], (unsigned long long)(t.off_cpu_ns / 1000000));
    CHECK(t.counts[PROF_EV_BLOCK] >= 10, "one block per sleep at least: %u", t.counts[PROF_EV_BLOCK]);
    CHECK(t.counts[PROF_EV_RUN] >= 10, "one run per wakeup at least: %u", t.counts[PROF_EV_RUN]);
    CHECK(t.off_cpu_ns >= 150000000ull && t.off_cpu_ns <= 600000000ull,
          "off CPU time is about the 200 ms it slept: %llu ns", (unsigned long long)t.off_cpu_ns);
}

static void test_heap_and_io(int fd)
{
    struct prof_config cfg = { .events = PROF_MASK_HEAP | PROF_MASK_IO, .max_depth = PROF_MAX_FRAMES };
    CHECK(prof_configure(fd, &cfg) == 0, "record the heap and transfer classes");
    mkdir("/tmp", 0755);
    CHECK(prof_start(fd, getpid()) == 0, "start on this process");
    /* Opening a file allocates kernel objects; writing and reading a
     * regular file produces transfers. */
    static char block[8192];
    memset(block, 'p', sizeof block);
    for (int i = 0; i < 40; i++) {
        int f = open("/tmp/proftest.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (f < 0)
            continue;
        write(f, block, sizeof block);
        lseek(f, 0, SEEK_SET);
        read(f, block, sizeof block);
        close(f);
    }
    prof_stop(fd);
    struct tally t = { 0 };
    tally(fd, getpid(), &t);
    printf("proftest: heap and io: %u allocations (%llu bytes), %u frees, %u transfers (%llu bytes)\n",
           t.counts[PROF_EV_ALLOC], (unsigned long long)t.alloc_bytes, t.counts[PROF_EV_FREE],
           t.counts[PROF_EV_IO], (unsigned long long)t.io_bytes);
    CHECK(t.counts[PROF_EV_SAMPLE] == 0, "the CPU class was switched off: %u samples", t.counts[PROF_EV_SAMPLE]);
    CHECK(t.counts[PROF_EV_ALLOC] >= 40, "the opens are recorded: %u", t.counts[PROF_EV_ALLOC]);
    CHECK(t.counts[PROF_EV_FREE] >= 20, "the closes are recorded: %u", t.counts[PROF_EV_FREE]);
    CHECK(t.counts[PROF_EV_IO] >= 80, "both transfers of every round: %u", t.counts[PROF_EV_IO]);
    CHECK(t.io_bytes >= 40ull * 2 * sizeof block / 2, "transferred bytes counted: %llu",
          (unsigned long long)t.io_bytes);
    unlink("/tmp/proftest.dat");
}

/* Feed a recorded run through a session and check the aggregation. */
static void test_session(int fd)
{
    struct prof_config cfg = { .events = PROF_MASK_ALL, .max_depth = PROF_MAX_FRAMES };
    prof_configure(fd, &cfg);
    struct prof_resolver *res = prof_resolver_new();
    CHECK(res != NULL, "resolver");
    prof_resolver_kernel(res);
    struct prof_stats st;
    prof_start(fd, getpid());
    prof_get_stats(fd, &st);
    struct prof_session *s = prof_session_new(res, st.period_ns);
    CHECK(s != NULL, "session");
    if (!s)
        return;
    deep(6, 0.4);
    void *blocks[32];
    for (int i = 0; i < 32; i++)
        blocks[i] = malloc(4096);
    for (int i = 0; i < 32; i++)
        free(blocks[i]);
    usleep(30000);
    prof_stop(fd);
    ssize_t n;
    size_t fed = 0;
    while ((n = prof_read_events(fd, buffer, READ_BYTES)) > 0)
        fed += prof_session_feed(s, buffer, (size_t)n);
    printf("proftest: session: %zu events, %llu samples, tree %d nodes, %zu symbols\n", fed,
           (unsigned long long)s->counts[PROF_EV_SAMPLE], s->view[PROF_VIEW_CPU].count,
           s->flat[PROF_VIEW_CPU].count);
    CHECK(fed == s->events && s->events > 100, "every record was consumed: %zu", fed);
    CHECK(s->view[PROF_VIEW_CPU].count > 3, "the CPU call tree has depth: %d", s->view[PROF_VIEW_CPU].count);

    struct prof_tree *t = &s->view[PROF_VIEW_CPU];
    prof_tree_sort(t);
    /* Every node holds the weight of its subtree, so a child can never be
     * wider than its parent and the root holds the total. */
    uint64_t sum_self = 0;
    int bad_parent = 0;
    for (int i = 1; i < t->count; i++) {
        sum_self += t->nodes[i].self;
        if (t->nodes[i].total > t->nodes[t->nodes[i].parent].total)
            bad_parent++;
    }
    sum_self += t->nodes[0].self;
    CHECK(bad_parent == 0, "no node outweighs its parent: %d", bad_parent);
    CHECK(sum_self == t->nodes[0].total, "the root holds every weight: %llu of %llu",
          (unsigned long long)sum_self, (unsigned long long)t->nodes[0].total);

    struct prof_flame_box *boxes = malloc((size_t)t->count * sizeof *boxes);
    size_t nboxes = prof_flame_layout(t, 0, 0, boxes, (size_t)t->count);
    CHECK(nboxes > 1 && boxes[0].node == 0 && boxes[0].start == 0 &&
          boxes[0].width == t->nodes[0].total, "the flame graph starts with the whole root");
    int overflow = 0;
    for (size_t i = 0; i < nboxes; i++)
        if (boxes[i].start + boxes[i].width > t->nodes[0].total)
            overflow++;
    CHECK(overflow == 0, "no box leaves the root's extent: %d", overflow);
    free(boxes);

    const char *names[PROF_MAX_FRAMES];
    int found_deep = 0;
    for (int i = 1; i < t->count && !found_deep; i++) {
        int k = prof_tree_path(t, i, names, PROF_MAX_FRAMES);
        for (int d = 0; d < k; d++)
            if (strcmp(names[d], "deep") == 0)
                found_deep++;
    }
    CHECK(found_deep > 0, "the recursive frames are in the tree");
    CHECK(s->nthreads > 0, "threads were seen: %zu", s->nthreads);
    prof_session_sort_threads(s);
    CHECK(s->threads[0].pid == (uint32_t)getpid(), "this process leads the thread table");
    prof_session_free(s);
    prof_resolver_free(res);
}

static void test_divider_and_overflow(int fd)
{
    struct prof_config cfg = { .events = PROF_MASK_CPU, .max_depth = PROF_MAX_FRAMES };
    prof_configure(fd, &cfg);
    CHECK(prof_set_divider(fd, 0) < 0 && errno == EINVAL, "divider 0 refused");
    CHECK(prof_set_divider(fd, 10) == 0, "divider 10");
    prof_start(fd, getpid());
    outer(0.5);
    prof_stop(fd);
    struct tally t = { 0 };
    tally(fd, getpid(), &t);
    printf("proftest: divider 10: %u samples\n", t.total);
    CHECK(t.total >= 20 && t.total <= 120, "about a tenth of the samples: %u", t.total);
    prof_set_divider(fd, 1);

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
    printf("proftest: overflow: %llu events, %llu dropped, %llu bytes pending of %u per CPU\n",
           (unsigned long long)st.events, (unsigned long long)st.dropped,
           (unsigned long long)st.pending, ring);
    CHECK(st.dropped > 0, "overflow is counted");
    CHECK(st.pending > ring / 2, "the rings are full: %llu", (unsigned long long)st.pending);
    memset(&t, 0, sizeof t);
    tally(fd, 0, &t);
    CHECK(t.total > 100, "the rings read back: %u events", t.total);
    prof_get_stats(fd, &st);
    CHECK(st.pending == 0, "rings drained");
    CHECK(prof_configure(fd, &(struct prof_config){ .max_depth = PROF_MAX_FRAMES + 1 }) < 0,
          "an impossible depth is refused");
}

int main(void)
{
    printf("proftest: pid %d\n", getpid());
    buffer = malloc(READ_BYTES);
    int fd = prof_open();
    CHECK(fd >= 0, "open /dev/profile: %s", strerror(errno));
    if (fd < 0 || !buffer)
        return 1;
    test_symbols();
    test_self_profile(fd);
    test_filter_and_kernel(fd);
    test_scheduler(fd);
    test_heap_and_io(fd);
    test_session(fd);
    test_divider_and_overflow(fd);
    close(fd);
    printf("proftest: %d failures\n", failures);
    return failures ? 1 : 0;
}
