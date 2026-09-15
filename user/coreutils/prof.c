/* prof: the command line front end of the system profiler.
 *
 *   prof [options] command [args...]   profile a command until it exits
 *   prof [options] -p pid              profile one process
 *   prof [options] -a                  profile every process
 *
 * Samples and the other events come from /dev/profile. User addresses are
 * resolved with the symbol table of the running binary and of the shared
 * libraries it mapped, kernel addresses with /dev/ksyms. Reports are the
 * flat profile, the hottest call chains, an ASCII flame graph, folded
 * stacks for an external renderer, per thread scheduling, kernel heap
 * activity with the allocations that were never freed, and completed
 * transfers. See prof(1) and docs/design/profile.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <time.h>
#include <minios/profile.h>

#define READ_BYTES 65536

static struct prof_resolver *res;
static struct prof_session *session;
static int top = 25;
static int want_chains, want_flame, want_folded, want_threads, want_heap, want_io, want_kernel;
static int flame_view = PROF_VIEW_CPU;
static int flame_width = 100;
static char *buffer;
static const char *output_path;

static const char *fmt_time(uint64_t ns, char *buf, size_t size)
{
    if (ns >= 1000000000ull)
        snprintf(buf, size, "%llu.%03llus", (unsigned long long)(ns / 1000000000ull),
                 (unsigned long long)(ns % 1000000000ull / 1000000ull));
    else if (ns >= 1000000)
        snprintf(buf, size, "%llu.%03llums", (unsigned long long)(ns / 1000000),
                 (unsigned long long)(ns % 1000000 / 1000));
    else if (ns >= 1000)
        snprintf(buf, size, "%lluus", (unsigned long long)(ns / 1000));
    else
        snprintf(buf, size, "%lluns", (unsigned long long)ns);
    return buf;
}

static const char *fmt_bytes(uint64_t b, char *buf, size_t size)
{
    if (b >= 1024ull * 1024)
        snprintf(buf, size, "%llu.%lluM", (unsigned long long)(b >> 20),
                 (unsigned long long)((b & ((1 << 20) - 1)) * 10 >> 20));
    else if (b >= 1024)
        snprintf(buf, size, "%llu.%lluK", (unsigned long long)(b >> 10),
                 (unsigned long long)((b & 1023) * 10 >> 10));
    else
        snprintf(buf, size, "%lluB", (unsigned long long)b);
    return buf;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void consume(int fd)
{
    ssize_t n;
    while ((n = prof_read_events(fd, buffer, READ_BYTES)) > 0)
        prof_session_feed(session, buffer, (size_t)n);
}

/* ---- reports ---- */

static void report_flat(int view, const char *title, int bytes_unit)
{
    struct prof_hist *h = &session->flat[view];
    if (!h->count)
        return;
    prof_hist_sort(h);
    char amount[32];
    printf("\n%s\n", title);
    printf("%7s %12s %8s  %s\n", "percent", bytes_unit ? "bytes" : "time", "events", "symbol");
    for (size_t i = 0; i < h->count && (int)i < top; i++) {
        struct prof_bucket *b = &h->buckets[i];
        double pct = h->weight ? 100.0 * (double)b->weight / (double)h->weight : 0.0;
        if (bytes_unit)
            fmt_bytes(b->weight, amount, sizeof amount);
        else
            fmt_time(b->weight, amount, sizeof amount);
        printf("%6.1f%% %12s %8u  %s%s\n", pct, amount, b->count, b->key,
               b->kernel == b->count ? " [kernel]" : b->kernel ? " [mixed]" : "");
    }
}

/* Every node that events ended in, deepest weight first, as a chain. */
struct chain_row {
    int node;
    uint64_t weight;
};

static int cmp_chain(const void *a, const void *b)
{
    const struct chain_row *x = a, *y = b;
    return x->weight == y->weight ? 0 : x->weight < y->weight ? 1 : -1;
}

static void print_path(struct prof_tree *t, int node, const char *sep)
{
    const char *names[PROF_MAX_FRAMES];
    int n = prof_tree_path(t, node, names, PROF_MAX_FRAMES);
    for (int i = 0; i < n; i++)
        printf("%s%s", i ? sep : "", names[i]);
}

static void report_chains(int view, const char *title, int bytes_unit)
{
    struct prof_tree *t = &session->view[view];
    if (t->count < 2)
        return;
    struct chain_row *rows = malloc((size_t)t->count * sizeof *rows);
    if (!rows)
        return;
    int n = 0;
    for (int i = 1; i < t->count; i++)
        if (t->nodes[i].self)
            rows[n++] = (struct chain_row){ i, t->nodes[i].self };
    qsort(rows, (size_t)n, sizeof *rows, cmp_chain);
    printf("\n%s\n", title);
    char amount[32];
    uint64_t total = t->nodes[0].total;
    for (int i = 0; i < n && i < top; i++) {
        double pct = total ? 100.0 * (double)rows[i].weight / (double)total : 0.0;
        printf("%6.1f%% %12s  ", pct,
               bytes_unit ? fmt_bytes(rows[i].weight, amount, sizeof amount)
                          : fmt_time(rows[i].weight, amount, sizeof amount));
        print_path(t, rows[i].node, " < ");
        printf("\n");
    }
    free(rows);
}

/* Folded stacks: one "frame;frame;frame weight" line per node that events
 * ended in, the input format of the usual flame graph renderers. */
static void report_folded(int view)
{
    struct prof_tree *t = &session->view[view];
    for (int i = 1; i < t->count; i++) {
        if (!t->nodes[i].self)
            continue;
        print_path(t, i, ";");
        printf(" %llu\n", (unsigned long long)t->nodes[i].self);
    }
}

/* An ASCII flame graph: one row per stack depth, each box as wide as its
 * share of the total and labelled with as much of the frame as fits. */
static void report_flame(int view, const char *title)
{
    struct prof_tree *t = &session->view[view];
    if (t->count < 2 || !t->nodes[0].total)
        return;
    prof_tree_sort(t);
    struct prof_flame_box *boxes = malloc((size_t)t->count * sizeof *boxes);
    char *row = malloc((size_t)flame_width + 1);
    if (!boxes || !row) {
        free(boxes);
        free(row);
        return;
    }
    size_t count = prof_flame_layout(t, 0, 0, boxes, (size_t)t->count);
    int depth_max = 0;
    for (size_t i = 0; i < count; i++)
        if (boxes[i].depth > depth_max)
            depth_max = boxes[i].depth;
    uint64_t total = t->nodes[0].total;
    printf("\n%s\n", title);
    for (int d = depth_max; d >= 0; d--) {
        memset(row, ' ', (size_t)flame_width);
        row[flame_width] = '\0';
        for (size_t i = 0; i < count; i++) {
            if (boxes[i].depth != d)
                continue;
            int x0 = (int)((double)boxes[i].start / (double)total * flame_width);
            int x1 = (int)((double)(boxes[i].start + boxes[i].width) / (double)total * flame_width);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (x0 >= flame_width)
                continue;
            if (x1 > flame_width)
                x1 = flame_width;
            const char *name = d == 0 ? "all"
                                      : prof_names_get(t->names, t->nodes[boxes[i].node].name);
            row[x0] = '[';
            for (int x = x0 + 1; x < x1 - 1; x++)
                row[x] = '-';
            if (x1 - 1 > x0)
                row[x1 - 1] = ']';
            for (int x = x0 + 1, k = 0; x < x1 - 1 && name[k]; x++, k++)
                row[x] = name[k];
        }
        printf("%s\n", row);
    }
    free(boxes);
    free(row);
}

static void report_threads(void)
{
    prof_session_sort_threads(session);
    printf("\nThreads, on CPU time measured when the scheduler class is recorded and "
           "estimated from samples otherwise\n");
    printf("%6s %6s %10s %10s %10s %8s %8s  %s\n", "PID", "TID", "on CPU", "off CPU", "ready",
           "blocks", "preempts", "name");
    char a[32], b[32], c[32];
    for (size_t i = 0; i < session->nthreads && (int)i < top; i++) {
        struct prof_thread_stat *t = &session->threads[i];
        uint64_t on = t->on_cpu_ns ? t->on_cpu_ns : t->samples * session->period_ns;
        printf("%6u %6u %10s %10s %10s %8llu %8llu  %s\n", t->pid, t->tid,
               fmt_time(on, a, sizeof a), fmt_time(t->off_cpu_ns, b, sizeof b),
               fmt_time(t->ready_ns, c, sizeof c), (unsigned long long)t->blocks,
               (unsigned long long)t->preempts, t->name ? t->name : "?");
    }
}

static void report_heap(void)
{
    char a[32], b[32], c[32];
    printf("\nKernel heap: %s allocated in %llu calls, %s freed, %s still held, peak %s\n",
           fmt_bytes(session->alloc_bytes, a, sizeof a),
           (unsigned long long)session->counts[PROF_EV_ALLOC],
           fmt_bytes(session->freed_bytes, b, sizeof b),
           fmt_bytes(session->live_bytes, c, sizeof c),
           fmt_bytes(session->live_peak, a, sizeof a));
    report_flat(PROF_VIEW_HEAP, "Allocation sites (bytes requested)", 1);
    int nodes[64];
    uint64_t bytes[64];
    size_t n = prof_session_leaks(session, nodes, bytes, 64);
    if (!n)
        return;
    printf("\nStill allocated when profiling stopped, by call site\n");
    for (size_t i = 0; i < n && (int)i < top; i++) {
        printf("%12s  ", fmt_bytes(bytes[i], a, sizeof a));
        print_path(&session->view[PROF_VIEW_HEAP], nodes[i], " < ");
        printf("\n");
    }
}

static void report_io(void)
{
    struct prof_hist *h = &session->flat[PROF_VIEW_IO];
    if (!h->count)
        return;
    uint64_t bytes = 0;
    for (size_t i = 0; i < h->count; i++)
        bytes += h->buckets[i].extra;
    char a[32], b[32];
    printf("\nTransfers: %llu completed, %s moved, %s of latency\n",
           (unsigned long long)session->counts[PROF_EV_IO],
           fmt_bytes(bytes, a, sizeof a), fmt_time(h->weight, b, sizeof b));
    prof_hist_sort(h);
    printf("%7s %12s %12s %8s  %s\n", "percent", "latency", "bytes", "count", "symbol");
    for (size_t i = 0; i < h->count && (int)i < top; i++) {
        struct prof_bucket *k = &h->buckets[i];
        double pct = h->weight ? 100.0 * (double)k->weight / (double)h->weight : 0.0;
        printf("%6.1f%% %12s %12s %8u  %s\n", pct, fmt_time(k->weight, a, sizeof a),
               fmt_bytes(k->extra, b, sizeof b), k->count, k->key);
    }
}

/* ---- driving a session ---- */

static void usage(void)
{
    fprintf(stderr,
            "usage: prof [-d seconds] [-e classes] [-n top] [-D divider] [-s depth]\n"
            "            [-k] [-c] [-g] [-F] [-t] [-m] [-i] command [args...]\n"
            "       prof [options] -p pid\n"
            "       prof [options] -a\n"
            "  -e cpu,sched,heap,io,all   event classes to record (default cpu)\n"
            "  -A bytes   record only allocations of at least this size\n"
            "  -L ns      record only transfers that took at least this long\n"
            "  -g flame graph   -F folded stacks   -c call chains\n"
            "  -t threads       -m kernel heap     -i transfers\n"
            "  -o path          export full JSON capture (-F: folded CPU stacks)\n");
    exit(2);
}

static uint32_t parse_classes(const char *list)
{
    uint32_t mask = 0;
    char copy[128];
    snprintf(copy, sizeof copy, "%s", list);
    for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        if (strcmp(tok, "cpu") == 0)
            mask |= PROF_MASK_CPU;
        else if (strcmp(tok, "sched") == 0)
            mask |= PROF_MASK_SCHED;
        else if (strcmp(tok, "heap") == 0)
            mask |= PROF_MASK_HEAP;
        else if (strcmp(tok, "io") == 0)
            mask |= PROF_MASK_IO;
        else if (strcmp(tok, "all") == 0)
            mask |= PROF_MASK_ALL;
        else
            usage();
    }
    return mask;
}

int main(int argc, char **argv)
{
    double duration = 0;
    pid_t pid = 0;
    int all_pids = 0;
    struct prof_config cfg = { .divider = 1, .events = PROF_MASK_CPU, .max_depth = PROF_MAX_FRAMES };
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "-k") == 0) want_kernel = 1;
        else if (strcmp(argv[i], "-c") == 0) want_chains = 1;
        else if (strcmp(argv[i], "-g") == 0) want_flame = 1;
        else if (strcmp(argv[i], "-F") == 0) want_folded = 1;
        else if (strcmp(argv[i], "-t") == 0) want_threads = 1;
        else if (strcmp(argv[i], "-m") == 0) want_heap = 1;
        else if (strcmp(argv[i], "-i") == 0) want_io = 1;
        else if (strcmp(argv[i], "-a") == 0) all_pids = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_path = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) duration = atof(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) top = atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) pid = atoi(argv[++i]);
        else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) cfg.divider = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) cfg.max_depth = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) flame_width = atoi(argv[++i]);
        else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) cfg.events = parse_classes(argv[++i]);
        else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) cfg.alloc_min = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) cfg.io_min_ns = (unsigned)atoi(argv[++i]);
        else usage();
    }
    if (!pid && !all_pids && i >= argc)
        usage();
    if ((pid || all_pids) && duration <= 0)
        duration = 5;
    /* Asking for a report implies recording what feeds it. */
    if (want_threads)
        cfg.events |= PROF_MASK_SCHED;
    if (want_heap)
        cfg.events |= PROF_MASK_HEAP;
    if (want_io)
        cfg.events |= PROF_MASK_IO;
    if (cfg.max_depth < 1 || cfg.max_depth > PROF_MAX_FRAMES)
        usage();

    int fd = prof_open();
    if (fd < 0) {
        fprintf(stderr, "prof: /dev/profile: %s\n", strerror(errno));
        return 1;
    }
    buffer = malloc(READ_BYTES);
    res = prof_resolver_new();
    if (!buffer || !res) {
        fprintf(stderr, "prof: out of memory\n");
        return 1;
    }
    prof_resolver_kernel(res);

    /* A command is started stopped, so that no work happens before the
     * first sample: the child waits for the gate to open. */
    pid_t child = 0;
    int gate[2] = { -1, -1 };
    const char *target = "all processes";
    if (!all_pids && !pid) {
        if (pipe(gate) < 0) {
            fprintf(stderr, "prof: pipe: %s\n", strerror(errno));
            return 1;
        }
        child = fork();
        if (child < 0) {
            fprintf(stderr, "prof: fork: %s\n", strerror(errno));
            return 1;
        }
        if (child == 0) {
            char go;
            close(gate[1]);
            read(gate[0], &go, 1);
            close(gate[0]);
            execvp(argv[i], argv + i);
            fprintf(stderr, "prof: %s: %s\n", argv[i], strerror(errno));
            _exit(127);
        }
        close(gate[0]);
        pid = child;
    }
    if (pid) {
        const char *name = prof_resolver_procname(res, pid);
        static char text[64];
        snprintf(text, sizeof text, "pid %d (%s)", pid, name ? name : "?");
        target = text;
    }

    if (prof_configure(fd, &cfg) < 0 || prof_start(fd, pid) < 0) {
        fprintf(stderr, "prof: start: %s\n", strerror(errno));
        return 1;
    }
    struct prof_stats st;
    prof_get_stats(fd, &st);
    session = prof_session_new(res, st.period_ns);
    if (!session) {
        fprintf(stderr, "prof: out of memory\n");
        return 1;
    }
    if (all_pids)
        session->exclude = getpid();

    int status = 0;
    if (child) {
        write(gate[1], "g", 1);
        close(gate[1]);
        for (;;) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            poll(&pfd, 1, 200);
            consume(fd);
            if (waitpid(child, &status, WNOHANG) == child)
                break;
        }
    } else {
        /* The deadline is wall clock: a poll that returns at once because
         * events are already waiting must not shorten the run. */
        uint64_t deadline = now_ms() + (uint64_t)(duration * 1000);
        for (uint64_t now = now_ms(); now < deadline; now = now_ms()) {
            uint64_t left = deadline - now;
            struct pollfd pfd = { fd, POLLIN, 0 };
            poll(&pfd, 1, left < 200 ? (int)left : 200);
            consume(fd);
        }
    }
    prof_stop(fd);
    consume(fd);
    prof_get_stats(fd, &st);

    if (output_path && prof_session_export(session, &st, output_path,
            want_folded ? PROF_EXPORT_FOLDED : PROF_EXPORT_JSON, flame_view) < 0) {
        fprintf(stderr, "prof: export %s: %s\n", output_path, strerror(errno));
        close(fd);
        return 1;
    }
    if (want_folded) {
        if (!output_path)
            report_folded(flame_view);
        close(fd);
        return child && WIFEXITED(status) ? WEXITSTATUS(status) : 0;
    }

    char span[32];
    printf("prof: %s, %llu of %llu events kept, %llu dropped, %s of wall time\n"
           "prof: %llu samples, %llu blocks, %llu wakeups, %llu allocations, %llu frees,"
           " %llu transfers\n",
           target, (unsigned long long)session->events, (unsigned long long)st.events,
           (unsigned long long)st.dropped,
           fmt_time(session->last_ns - session->first_ns, span, sizeof span),
           (unsigned long long)session->counts[PROF_EV_SAMPLE],
           (unsigned long long)session->counts[PROF_EV_BLOCK],
           (unsigned long long)session->counts[PROF_EV_RUN],
           (unsigned long long)session->counts[PROF_EV_ALLOC],
           (unsigned long long)session->counts[PROF_EV_FREE],
           (unsigned long long)session->counts[PROF_EV_IO]);
    if (session->locked_samples)
        printf("prof: %d kernel samples were taken with interrupts disabled and are charged to the"
               " code that held the lock\n", session->locked_samples);
    report_flat(PROF_VIEW_CPU, "Flat profile (CPU time)", 0);
    if (want_chains)
        report_chains(PROF_VIEW_CPU, "Call chains by CPU time", 0);
    if (want_flame)
        report_flame(PROF_VIEW_CPU, "Flame graph (CPU time, widest first)");
    if (session->counts[PROF_EV_RUN]) {
        report_flat(PROF_VIEW_OFFCPU, "Off CPU profile (time blocked, by where it blocked)", 0);
        if (want_chains)
            report_chains(PROF_VIEW_OFFCPU, "Call chains by off CPU time", 0);
    }
    if (want_threads)
        report_threads();
    if (want_heap)
        report_heap();
    if (want_io)
        report_io();
    (void)want_kernel;
    close(fd);
    if (child)
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return 0;
}
