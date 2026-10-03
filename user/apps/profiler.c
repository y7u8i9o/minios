/* profiler: the graphical front end of the system profiler, grown out of
 * the Profile tab of sysmon.
 *
 * It records the event classes selected in the toolbar through
 * /dev/profile, feeds every record to a session from minios/profile.h and
 * shows the result four ways: a flame graph of the call tree that can be
 * zoomed by clicking a frame, a table of the hottest frames, per thread
 * scheduling, the kernel heap with the allocations that were never freed,
 * and completed transfers. Views update while recording runs.
 *
 *   profiler [-p pid]     preselect a process, as sysmon does
 *   profiler -r seconds   record at once and report when the time is up,
 *                         which is how the boot test drives it */
#include <minios/local.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ipc.h>
#include <gui/app.h>
#include <gui/model.h>
#include <minios/profile.h>

#define READ_BYTES 65536
#define MAX_PROCS 64
#define TABLE_ROWS 300
#define ROW_HEIGHT 16

static struct app *app;
static struct widget *flame_canvas, *flame_info, *status, *target_box, *view_box;
static struct widget *sym_table, *thread_table, *heap_table, *io_table;
static struct widget *class_cpu, *class_sched, *class_heap, *class_io, *depth_spin;

static int fd = -1;
static struct watch *watch;
static struct prof_resolver *res;
static struct prof_session *session;
static char *buffer;
static int view = PROF_VIEW_CPU;
static int zoom_node;               /* root of the drawn subtree */
static int preselect_pid;
static int running;
static int auto_seconds;
static struct widget *break_table, *break_info, *search_box, *search_info, *divider_spin;
static struct widget *start_button, *stop_button, *export_info;
static struct prof_breakdown *break_rows;
static size_t nbreak, break_cap;
static char capture_error[160];
static const char *auto_export;
static int test_ui;
static void check_ui(void);
static void breakdown_refresh(void);
static void search_refresh(void);

/* One row of the process list behind the target selector. */
static struct { int pid; char name[32]; } procs[MAX_PROCS];
static int nprocs;

/* ---- helpers ---- */

static int view_uses_bytes(int v)
{
    return v == PROF_VIEW_HEAP;
}

static const char *fmt_time(uint64_t ns, char *buf, size_t size)
{
    if (ns >= 1000000000ull)
        snprintf(buf, size, "%llu.%02llus", (unsigned long long)(ns / 1000000000ull),
                 (unsigned long long)(ns % 1000000000ull / 10000000ull));
    else if (ns >= 1000000)
        snprintf(buf, size, "%llums", (unsigned long long)(ns / 1000000));
    else if (ns >= 1000)
        snprintf(buf, size, "%lluus", (unsigned long long)(ns / 1000));
    else
        snprintf(buf, size, "%lluns", (unsigned long long)ns);
    return buf;
}

static const char *fmt_bytes(uint64_t b, char *buf, size_t size)
{
    if (b >= 1024ull * 1024)
        snprintf(buf, size, "%lluM", (unsigned long long)(b >> 20));
    else if (b >= 1024)
        snprintf(buf, size, "%lluK", (unsigned long long)(b >> 10));
    else
        snprintf(buf, size, "%llu", (unsigned long long)b);
    return buf;
}

static const char *fmt_amount(int v, uint64_t weight, char *buf, size_t size)
{
    return view_uses_bytes(v) ? fmt_bytes(weight, buf, size) : fmt_time(weight, buf, size);
}

static struct prof_tree *tree(void)
{
    return &session->view[view];
}

/* ---- the flame graph ---- */

static struct prof_flame_box *boxes;
static size_t nboxes, boxes_cap;
static int hover_box = -1;

/* A stable colour per frame: warm for user code, cool for the kernel, so
 * the two are told apart at a glance without a legend. */
static uint32_t frame_color(const char *name, int kernel)
{
    unsigned h = 2166136261u;
    while (*name) {
        h ^= (unsigned char)*name++;
        h *= 16777619u;
    }
    unsigned a = (h >> 4) & 0x3f, b = (h >> 12) & 0x3f;
    if (kernel)
        return (uint32_t)((0x30 + a) << 16 | (0x70 + b) << 8 | 0xb0);
    return (uint32_t)(0xc0 << 16 | (0x60 + a) << 8 | (0x30 + b));
}

static void flame_rebuild(void)
{
    struct prof_tree *t = tree();
    if (zoom_node >= t->count)
        zoom_node = 0;
    if ((size_t)t->count > boxes_cap) {
        size_t cap = (size_t)t->count + 64;
        struct prof_flame_box *b = realloc(boxes, cap * sizeof *b);
        if (!b)
            return;
        boxes = b;
        boxes_cap = cap;
    }
    prof_tree_sort(t);
    nboxes = prof_flame_layout(t, zoom_node, 0, boxes, boxes_cap);
    hover_box = -1;
    breakdown_refresh();
    search_refresh();
}

static int box_at(int x, int y, int width, int height)
{
    struct prof_tree *t = tree();
    if (!nboxes || !boxes)
        return -1;
    uint64_t total = t->nodes[zoom_node].total;
    if (!total)
        return -1;
    for (size_t i = 0; i < nboxes; i++) {
        int row = height - (boxes[i].depth + 1) * ROW_HEIGHT;
        if (y < row || y >= row + ROW_HEIGHT)
            continue;
        int x0 = (int)((double)boxes[i].start / (double)total * width);
        int x1 = (int)((double)(boxes[i].start + boxes[i].width) / (double)total * width);
        if (x >= x0 && x < x1)
            return (int)i;
    }
    return -1;
}

static int flame_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    struct prof_tree *t = tree();
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_FIELD]);
    uint64_t total = nboxes ? t->nodes[zoom_node].total : 0;
    if (!total) {
        painter_text(p, 8, w->h / 2, "No samples yet. Choose the classes to record and press Start.",
                     p->theme->color[TC_TEXT_DISABLED]);
        return 1;
    }
    char label[160], amount[32];
    for (size_t i = 0; i < nboxes; i++) {
        int row = w->h - (boxes[i].depth + 1) * ROW_HEIGHT;
        if (row + ROW_HEIGHT < 0)
            continue;
        int x0 = (int)((double)boxes[i].start / (double)total * w->w);
        int x1 = (int)((double)(boxes[i].start + boxes[i].width) / (double)total * w->w);
        if (x1 - x0 < 1)
            continue;
        const struct prof_node *n = &t->nodes[boxes[i].node];
        const char *name = boxes[i].node == zoom_node && !zoom_node
                               ? "all"
                               : prof_names_get(t->names, n->name);
        uint32_t color = frame_color(name, n->kernel);
        const char *query = widget_text(search_box);
        if (*query && boxes[i].node && strstr(name, query))
            color = 0xdd66dd;
        if ((int)i == hover_box)
            color |= 0x202020;
        painter_fill(p, x0, row, x1 - x0 - 1, ROW_HEIGHT - 1, color);
        if (x1 - x0 > 24) {
            snprintf(label, sizeof label, "%s", name);
            int max = (x1 - x0 - 6);
            while (label[0] && painter_text_width(p, label, -1) > max)
                label[strlen(label) - 1] = '\0';
            painter_text(p, x0 + 3, row + 2, label,
                         n->kernel ? 0x00ffffff : 0x00000000);
        }
    }
    (void)amount;
    return 1;
}

static void flame_describe(int index)
{
    struct prof_tree *t = tree();
    char text[384], amount[32], self[32];
    if (index < 0 || (size_t)index >= nboxes) {
        uint64_t total = t->nodes[zoom_node].total;
        snprintf(text, sizeof text, "%s: %s in %d frames%s",
                 view == PROF_VIEW_CPU ? "CPU time" : view == PROF_VIEW_OFFCPU ? "Off CPU time"
                 : view == PROF_VIEW_HEAP ? "Kernel heap" : "Transfers",
                 fmt_amount(view, total, amount, sizeof amount), t->count - 1,
                 zoom_node ? ", zoomed (click the bottom frame to widen)" : "");
        widget_set_text(flame_info, text);
        return;
    }
    const struct prof_node *n = &t->nodes[boxes[index].node];
    uint64_t total = t->nodes[zoom_node].total;
    double pct = total ? 100.0 * (double)boxes[index].width / (double)total : 0.0;
    char extra[64] = "";
    if (view == PROF_VIEW_HEAP)
        snprintf(extra, sizeof extra, ", %s still held",
                 fmt_bytes(n->extra, amount, sizeof amount));
    else if (view == PROF_VIEW_IO)
        snprintf(extra, sizeof extra, ", %s moved", fmt_bytes(n->extra, amount, sizeof amount));
    snprintf(text, sizeof text, "%s  %.1f%% of view  inclusive %s  self %s  %u self events%s  [%s]",
             prof_names_get(t->names, n->name), pct,
             fmt_amount(view, boxes[index].width, amount, sizeof amount),
             fmt_amount(view, n->self, self, sizeof self), n->count, extra,
             n->kernel ? "kernel" : "user");
    widget_set_text(flame_info, text);
}

static int flame_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    int index = box_at(c->x, c->y, w->w, w->h);
    struct prof_tree *t = tree();
    if (c->button == 2 || (index >= 0 && boxes[index].node == zoom_node)) {
        /* The right button, and a click on the frame at the bottom, widen
         * the view again one level at a time. */
        zoom_node = zoom_node ? t->nodes[zoom_node].parent : 0;
        if (zoom_node < 0)
            zoom_node = 0;
    } else if (index >= 0) {
        zoom_node = boxes[index].node;
    }
    flame_rebuild();
    flame_describe(-1);
    widget_invalidate(w);
    return 1;
}

static int flame_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    int index = box_at(c->x, c->y, w->w, w->h);
    if (index == hover_box)
        return 1;
    hover_box = index;
    flame_describe(index);
    widget_invalidate(w);
    return 1;
}

/* ---- tables ---- */

static int m_child(struct model *m, int parent, int index) { return index; }

static struct prof_hist *hist_of(int v) { return &session->flat[v]; }

static int sym_rows(struct model *m, int parent)
{
    if (parent >= 0)
        return 0;
    size_t n = hist_of(view)->count;
    return n < TABLE_ROWS ? (int)n : TABLE_ROWS;
}
static int sym_columns(struct model *m) { return 5; }
static const char *sym_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct prof_hist *h = hist_of(view);
    if ((size_t)row >= h->count)
        return "";
    struct prof_bucket *b = &h->buckets[row];
    switch (col) {
    case 0: snprintf(buf, size, "%.1f", h->weight ? 100.0 * (double)b->weight / (double)h->weight : 0.0); return buf;
    case 1: return fmt_amount(view, b->weight, buf, size);
    case 2: snprintf(buf, size, "%u", b->count); return buf;
    case 3: return b->key;
    default: return b->kernel == b->count ? "kernel" : b->kernel ? "mixed" : "user";
    }
}
static const char *sym_header(struct model *m, int col)
{
    static const char *const names[] = { "%", "Amount", "Events", "Frame", "Mode" };
    return names[col];
}
static struct model sym_model = { sym_rows, m_child, sym_columns, sym_cell, sym_header, NULL, NULL, NULL };

static int thread_rows(struct model *m, int parent)
{
    if (parent >= 0)
        return 0;
    return session->nthreads < TABLE_ROWS ? (int)session->nthreads : TABLE_ROWS;
}
static int thread_columns(struct model *m) { return 8; }
static const char *thread_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    if ((size_t)row >= session->nthreads)
        return "";
    struct prof_thread_stat *t = &session->threads[row];
    uint64_t on = t->on_cpu_ns ? t->on_cpu_ns : t->samples * session->period_ns;
    switch (col) {
    case 0: snprintf(buf, size, "%u", t->pid); return buf;
    case 1: snprintf(buf, size, "%u", t->tid); return buf;
    case 2: return t->name ? t->name : "?";
    case 3: return fmt_time(on, buf, size);
    case 4: return fmt_time(t->off_cpu_ns, buf, size);
    case 5: return fmt_time(t->ready_ns, buf, size);
    case 6: snprintf(buf, size, "%llu", (unsigned long long)t->blocks); return buf;
    default: snprintf(buf, size, "%llu", (unsigned long long)t->preempts); return buf;
    }
}
static const char *thread_header(struct model *m, int col)
{
    static const char *const names[] = { "PID", "TID", "Name", "On CPU", "Off CPU", "Ready", "Blocks", "Preempts" };
    return names[col];
}
static struct model thread_model = { thread_rows, m_child, thread_columns, thread_cell, thread_header, NULL, NULL, NULL };

/* The heap table lists the call sites that still hold memory. */
static int leak_nodes[TABLE_ROWS];
static uint64_t leak_bytes[TABLE_ROWS];
static size_t nleaks;

static int heap_rows(struct model *m, int parent) { return parent < 0 ? (int)nleaks : 0; }
static int heap_columns(struct model *m) { return 3; }
static const char *heap_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    if ((size_t)row >= nleaks)
        return "";
    struct prof_tree *t = &session->view[PROF_VIEW_HEAP];
    const struct prof_node *n = &t->nodes[leak_nodes[row]];
    switch (col) {
    case 0: return fmt_bytes(leak_bytes[row], buf, size);
    case 1: return fmt_bytes(n->total, buf, size);
    default: {
        const char *names[PROF_MAX_FRAMES];
        int k = prof_tree_path(t, leak_nodes[row], names, PROF_MAX_FRAMES);
        size_t used = 0;
        buf[0] = '\0';
        for (int i = k - 1; i >= 0 && used < size - 1; i--)
            used += (size_t)snprintf(buf + used, size - used, "%s%s", used ? " < " : "", names[i]);
        return buf;
    }
    }
}
static const char *heap_header(struct model *m, int col)
{
    static const char *const names[] = { "Still held", "Allocated", "Call site" };
    return names[col];
}
static struct model heap_model = { heap_rows, m_child, heap_columns, heap_cell, heap_header, NULL, NULL, NULL };

static int io_rows(struct model *m, int parent)
{
    if (parent >= 0)
        return 0;
    size_t n = session->flat[PROF_VIEW_IO].count;
    return n < TABLE_ROWS ? (int)n : TABLE_ROWS;
}
static int io_columns(struct model *m) { return 4; }
static const char *io_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct prof_hist *h = &session->flat[PROF_VIEW_IO];
    if ((size_t)row >= h->count)
        return "";
    struct prof_bucket *b = &h->buckets[row];
    switch (col) {
    case 0: return fmt_time(b->weight, buf, size);
    case 1: return fmt_bytes(b->extra, buf, size);
    case 2: snprintf(buf, size, "%u", b->count); return buf;
    default: return b->key;
    }
}
static const char *io_header(struct model *m, int col)
{
    static const char *const names[] = { "Latency", "Bytes", "Transfers", "Frame" };
    return names[col];
}
static struct model io_model = { io_rows, m_child, io_columns, io_cell, io_header, NULL, NULL, NULL };

/* The selected call site is the root of the zoomed graph. Direct callees
 * and self are disjoint, so the breakdown always sums to its inclusive cost. */
static int break_count(struct model *m, int parent) { return parent < 0 ? (int)nbreak : 0; }
static int break_columns(struct model *m) { return 5; }
static const char *break_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    if (row < 0 || (size_t)row >= nbreak)
        return "";
    struct prof_breakdown *b = &break_rows[row];
    struct prof_tree *t = tree();
    switch (col) {
    case 0: return b->node == zoom_node ? "[self]" : prof_names_get(t->names, t->nodes[b->node].name);
    case 1: snprintf(buf, size, "%.1f", t->nodes[zoom_node].total ?
                    100.0 * (double)b->total / t->nodes[zoom_node].total : 0.0); return buf;
    case 2: return fmt_amount(view, b->total, buf, size);
    case 3: return fmt_amount(view, b->self, buf, size);
    default: return view == PROF_VIEW_HEAP || view == PROF_VIEW_IO ? fmt_bytes(b->extra, buf, size) : "-";
    }
}
static const char *break_header(struct model *m, int col)
{
    static const char *const names[] = { "Self / direct callee", "% selected", "Inclusive", "Self", "Held / I/O bytes" };
    return names[col];
}
static struct model break_model = { break_count, m_child, break_columns, break_cell, break_header, NULL, NULL, NULL };

static void breakdown_refresh(void)
{
    struct prof_tree *t = tree();
    nbreak = 0;
    if ((size_t)t->count > break_cap) {
        struct prof_breakdown *rows = realloc(break_rows, (size_t)t->count * sizeof *rows);
        if (!rows) {
            widget_set_text(break_info, "Cannot allocate function breakdown");
            view_refresh(break_table);
            return;
        }
        break_rows = rows;
        break_cap = (size_t)t->count;
    }
    nbreak = prof_tree_breakdown(t, zoom_node, break_rows, break_cap);
    const struct prof_node *n = &t->nodes[zoom_node];
    char text[512], total[32], self[32];
    const char *caller = n->parent > 0 ? prof_names_get(t->names, t->nodes[n->parent].name) : "all";
    snprintf(text, sizeof text, "Breakdown: %s | caller: %s | inclusive %s | self %s | %.1f%% of capture",
             zoom_node ? prof_names_get(t->names, n->name) : "all", caller,
             fmt_amount(view, n->total, total, sizeof total), fmt_amount(view, n->self, self, sizeof self),
             t->nodes[0].total ? 100.0 * (double)n->total / t->nodes[0].total : 0.0);
    widget_set_text(break_info, text);
    view_refresh(break_table);
}

static void search_refresh(void)
{
    char text[128], amount[32];
    const char *query = widget_text(search_box);
    uint64_t matched = prof_tree_match_weight(tree(), zoom_node, query);
    uint64_t total = tree()->nodes[zoom_node].total;
    snprintf(text, sizeof text, "%s%s (%.1f%% of view)", *query ? "Matched: " : "Search: ",
             fmt_amount(view, matched, amount, sizeof amount), total ? 100.0 * (double)matched / total : 0.0);
    widget_set_text(search_info, text);
}

static int on_search(struct widget *w, void *args, void *arg)
{
    search_refresh();
    widget_invalidate(flame_canvas);
    return 1;
}

static int on_navigate(struct widget *w, void *args, void *arg)
{
    zoom_node = arg && zoom_node ? tree()->nodes[zoom_node].parent : 0;
    flame_rebuild();
    flame_describe(-1);
    widget_invalidate(flame_canvas);
    return 1;
}

static int on_break_activate(struct widget *w, void *args, void *arg)
{
    int row = ((struct sig_select *)args)->index;
    if (row >= 0 && (size_t)row < nbreak && break_rows[row].node != zoom_node) {
        zoom_node = break_rows[row].node;
        flame_rebuild();
        flame_describe(-1);
        widget_invalidate(flame_canvas);
    }
    return 1;
}

/* ---- recording ---- */

static void refresh_views(void)
{
    for (int v = 0; v < PROF_VIEW_COUNT; v++)
        prof_hist_sort(&session->flat[v]);
    prof_session_sort_threads(session);
    nleaks = prof_session_leaks(session, leak_nodes, leak_bytes, TABLE_ROWS);
    flame_rebuild();
    flame_describe(-1);
    widget_invalidate(flame_canvas);
    view_refresh(sym_table);
    view_refresh(thread_table);
    view_refresh(heap_table);
    view_refresh(io_table);

    struct prof_stats st = { 0 };
    if (fd >= 0)
        prof_get_stats(fd, &st);
    char text[220], span[32], held[32];
    snprintf(text, sizeof text,
             "%s | %llu events: %llu samples, %llu switches, %llu allocations, %llu transfers | "
             "%llu dropped | %s recorded | %s held by the kernel heap",
             running ? "recording" : "stopped", (unsigned long long)session->events,
             (unsigned long long)session->counts[PROF_EV_SAMPLE],
             (unsigned long long)session->counts[PROF_EV_BLOCK],
             (unsigned long long)session->counts[PROF_EV_ALLOC],
             (unsigned long long)session->counts[PROF_EV_IO],
             (unsigned long long)st.dropped,
             fmt_time(session->last_ns - session->first_ns, span, sizeof span),
             fmt_bytes(session->live_bytes, held, sizeof held));
    widget_set_text(status, capture_error[0] ? capture_error : text);
}

static void drain(void)
{
    ssize_t n;
    /* Yield to input and painting even under a sustained event stream.
     * A stopped capture is finite and must be drained completely. */
    for (int batches = 0; fd >= 0 && (!running || batches < 16); batches++) {
        n = prof_read_events(fd, buffer, READ_BYTES);
        if (n <= 0) {
            if (n < 0)
                snprintf(capture_error, sizeof capture_error, "Read failed: %s", strerror(errno));
            break;
        }
        prof_session_feed(session, buffer, (size_t)n);
    }
}

static void on_readable(int f, int revents, void *arg)
{
    drain();
}

static void tick(void *arg)
{
    if (running) {
        drain();
        refresh_views();
    }
}

/* The process list behind the target selector. */
static void load_procs(void)
{
    FILE *f = fopen("/dev/proc", "r");
    nprocs = 0;
    if (!f)
        return;
    char line[256];
    while (nprocs < MAX_PROCS && fgets(line, sizeof line, f)) {
        char *end;
        long pid = strtol(line, &end, 10);
        if (end == line || pid <= 0)
            continue;
        char *tok = end;
        for (int col = 0; col < 6; col++) {
            while (*tok == ' ')
                tok++;
            while (*tok && *tok != ' ')
                tok++;
        }
        while (*tok == ' ')
            tok++;
        tok[strcspn(tok, "\n")] = '\0';
        procs[nprocs].pid = (int)pid;
        snprintf(procs[nprocs].name, sizeof procs[nprocs].name, "%s", tok);
        nprocs++;
    }
    fclose(f);
    combobox_clear(target_box);
    combobox_add(target_box, "All processes");
    char item[64];
    int select = 0;
    for (int i = 0; i < nprocs; i++) {
        snprintf(item, sizeof item, "%d %s", procs[i].pid, procs[i].name);
        combobox_add(target_box, item);
        if (procs[i].pid == preselect_pid)
            select = i + 1;
    }
    combobox_select(target_box, select);
}

static int on_start(struct widget *w, void *args, void *arg)
{
    if (running)
        return 1;
    capture_error[0] = '\0';
    if (fd < 0) {
        fd = prof_open();
        if (fd < 0) {
            widget_set_text(status, "cannot open /dev/profile");
            return 1;
        }
        watch = app_watch_fd(app, fd, POLLIN, on_readable, NULL);
    }
    struct prof_config cfg = {
        .events = (uint32_t)((class_cpu->value ? PROF_MASK_CPU : 0) |
                             (class_sched->value ? PROF_MASK_SCHED : 0) |
                             (class_heap->value ? PROF_MASK_HEAP : 0) |
                             (class_io->value ? PROF_MASK_IO : 0)),
        .max_depth = (uint32_t)depth_spin->value,
        .divider = (uint32_t)divider_spin->value,
    };
    if (!cfg.events)
        cfg.events = PROF_MASK_CPU;
    if (prof_configure(fd, &cfg) < 0) {
        widget_set_text(status, "Cannot configure recording");
        return 1;
    }
    int index = target_box->value;
    pid_t pid = index > 0 && index <= nprocs ? procs[index - 1].pid : 0;
    struct prof_stats st = { 0 };
    if (prof_get_stats(fd, &st) < 0) {
        widget_set_text(status, "Cannot read recording configuration");
        return 1;
    }
    struct prof_session *next = prof_session_new(res, st.period_ns);
    if (!next) {
        widget_set_text(status, "out of memory");
        return 1;
    }
    if (!pid)
        next->exclude = getpid();    /* do not profile the profiler */
    if (prof_start(fd, pid) < 0) {
        prof_session_free(next);
        widget_set_text(status, "cannot start recording");
        return 1;
    }
    prof_session_free(session);
    session = next;
    zoom_node = 0;
    running = 1;
    widget_set_enabled(start_button, 0);
    widget_set_enabled(stop_button, 1);
    widget_set_text(export_info, "");
    printf("profiler: recording %s\n", pid ? "one process" : "every process");
    fflush(stdout);
    refresh_views();
    return 1;
}

static int on_stop(struct widget *w, void *args, void *arg)
{
    if (fd >= 0 && running) {
        if (prof_stop(fd) < 0) {
            widget_set_text(status, "Cannot stop recording");
            return 1;
        }
        running = 0;
        drain();
    }
    running = 0;
    widget_set_enabled(start_button, 1);
    widget_set_enabled(stop_button, 0);
    refresh_views();
    struct prof_tree *t = &session->view[PROF_VIEW_CPU];
    printf("profiler: %llu events, %llu samples, %d nodes in the CPU tree, %zu threads,"
           " %llu allocations, %llu transfers\n",
           (unsigned long long)session->events,
           (unsigned long long)session->counts[PROF_EV_SAMPLE], t->count - 1, session->nthreads,
           (unsigned long long)session->counts[PROF_EV_ALLOC],
           (unsigned long long)session->counts[PROF_EV_IO]);
    fflush(stdout);
    return 1;
}

/* Exports are stopped snapshots. Stopping first also prevents the export
 * writes from becoming part of their own heap or I/O profile. */
static int save_capture(const char *path, int format)
{
    on_stop(NULL, NULL, NULL);
    if (running)
        return -1;
    struct prof_stats st;
    int have_stats = fd >= 0 && prof_get_stats(fd, &st) == 0;
    int rc = prof_session_export(session, have_stats ? &st : NULL, path, format, view);
    char text[384];
    snprintf(text, sizeof text, rc < 0 ? "Export failed: %s" : "Saved %s",
             rc < 0 ? strerror(errno) : path);
    widget_set_text(export_info, text);
    return rc;
}

static int on_export(struct widget *w, void *args, void *arg)
{
    int format = arg ? PROF_EXPORT_FOLDED : PROF_EXPORT_JSON;
    char path[256];
    snprintf(path, sizeof path, "/home/profile.%s", format == PROF_EXPORT_JSON ? "json" : "folded");
    if (!app_prompt(app, "Export capture", "Save path (capture stops on export):", path, sizeof path))
        return 1;
    if (access(path, F_OK) == 0) {
        static const char *const buttons[] = { "Cancel", "Replace" };
        if (app_dialog(app, "Replace export", path, buttons, 2) != 1)
            return 1;
    }
    save_capture(path, format);
    return 1;
}

static void auto_stop(void *arg)
{
    on_stop(NULL, NULL, NULL);
    if (auto_export)
        printf("profiler: automatic export %s\n", save_capture(auto_export, PROF_EXPORT_JSON) == 0 ? "saved" : "failed");
    if (test_ui)
        check_ui();
    fflush(stdout);
}

static int on_reset(struct widget *w, void *args, void *arg)
{
    on_stop(NULL, NULL, NULL);
    if (running)
        return 1;
    prof_session_reset(session);
    capture_error[0] = '\0';
    widget_set_text(export_info, "");
    zoom_node = 0;
    load_procs();
    refresh_views();
    return 1;
}

static int on_view(struct widget *w, void *args, void *arg)
{
    view = w->value;
    zoom_node = 0;
    refresh_views();
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            preselect_pid = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
            auto_seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            auto_export = argv[++i];
        else if (strcmp(argv[i], "--test-ui") == 0)
            test_ui = 1;
    buffer = malloc(READ_BYTES);
    res = prof_resolver_new();
    if (!buffer || !res)
        return 1;
    prof_resolver_kernel(res);
    session = prof_session_new(res, 1000000);
    if (!session)
        return 1;

    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 860, 600, "profiler");
    if (!win)
        return 1;
    struct widget *bar = toolbar_new(win);
    start_button = button_new(bar, "Start");
    stop_button = button_new(bar, "Stop");
    widget_connect(start_button, "clicked", on_start, NULL);
    widget_connect(stop_button, "clicked", on_stop, NULL);
    widget_set_enabled(stop_button, 0);
    widget_connect(button_new(bar, "Reset"), "clicked", on_reset, NULL);
    target_box = combobox_new(bar);
    view_box = combobox_new(bar);
    combobox_add(view_box, "CPU time");
    combobox_add(view_box, "Off CPU time");
    combobox_add(view_box, "Kernel heap");
    combobox_add(view_box, "Transfers");
    combobox_select(view_box, 0);
    widget_connect(view_box, "changed", on_view, NULL);
    widget_connect(button_new(bar, "Export JSON"), "clicked", on_export, NULL);
    widget_connect(button_new(bar, "Export folded"), "clicked", on_export, (void *)1);
    bar = toolbar_new(win);
    class_cpu = checkbox_new(bar, "CPU");
    class_cpu->value = 1;
    class_sched = checkbox_new(bar, "Scheduler");
    class_sched->value = 1;
    class_heap = checkbox_new(bar, "Heap");
    class_io = checkbox_new(bar, "I/O");
    label_new(bar, "Depth");
    depth_spin = spinner_new(bar, 1, PROF_MAX_FRAMES, PROF_MAX_FRAMES);
    label_new(bar, "Sample interval (ms)");
    divider_spin = spinner_new(bar, 1, 1000, 1);
    widget_set_tip(divider_spin, "Applies on Start. Larger intervals reduce CPU sampling overhead.");

    struct widget *tabs = tabs_new(win);
    struct widget *page = tabs_add(tabs, "Flame graph");
    bar = toolbar_new(page);
    struct widget *up = button_new(bar, "Up"), *all = button_new(bar, "All frames");
    widget_set_id(up, "up");
    widget_set_id(all, "all");
    widget_connect(up, "clicked", on_navigate, (void *)1);
    widget_connect(all, "clicked", on_navigate, NULL);
    label_new(bar, "Find function");
    search_box = textfield_new(bar, "");
    widget_set_hint(search_box, 180, 24);
    widget_connect(search_box, "changed", on_search, NULL);
    search_info = label_new(bar, "");
    flame_info = label_new(page, "Press Start to record.");
    struct widget *split = splitpane_new(page, 1);
    splitpane_set_position(split, 220);
    flame_canvas = canvas_new(split);
    widget_connect(flame_canvas, "paint", flame_paint, NULL);
    widget_connect(flame_canvas, "press", flame_press, NULL);
    widget_connect(flame_canvas, "motion", flame_motion, NULL);
    struct widget *details = box_new(split, 1);
    break_info = label_new(details, "");
    break_table = table_new(details);
    view_set_model(break_table, &break_model);
    table_set_column_width(break_table, 0, 280);
    for (int c = 1; c < 5; c++)
        table_set_column_width(break_table, c, 125);
    widget_connect(break_table, "activate", on_break_activate, NULL);

    page = tabs_add(tabs, "Frames");
    sym_table = table_new(page);
    view_set_model(sym_table, &sym_model);
    table_set_column_width(sym_table, 0, 60);
    table_set_column_width(sym_table, 1, 90);
    table_set_column_width(sym_table, 2, 80);
    table_set_column_width(sym_table, 3, 420);
    table_set_column_width(sym_table, 4, 70);

    page = tabs_add(tabs, "Threads");
    thread_table = table_new(page);
    view_set_model(thread_table, &thread_model);
    table_set_column_width(thread_table, 0, 60);
    table_set_column_width(thread_table, 1, 60);
    table_set_column_width(thread_table, 2, 150);
    for (int c = 3; c < 8; c++)
        table_set_column_width(thread_table, c, 90);

    page = tabs_add(tabs, "Heap");
    heap_table = table_new(page);
    view_set_model(heap_table, &heap_model);
    table_set_column_width(heap_table, 0, 90);
    table_set_column_width(heap_table, 1, 90);
    table_set_column_width(heap_table, 2, 600);

    page = tabs_add(tabs, "Transfers");
    io_table = table_new(page);
    view_set_model(io_table, &io_model);
    table_set_column_width(io_table, 0, 100);
    table_set_column_width(io_table, 1, 100);
    table_set_column_width(io_table, 2, 90);
    table_set_column_width(io_table, 3, 420);

    export_info = label_new(win, "");
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    load_procs();
    refresh_views();
    app_timer_add(app, 500, 1, tick, NULL);
    if (auto_seconds > 0) {
        /* Record every class without waiting for the toolbar, so a script
         * or a boot test gets one complete session. */
        class_heap->value = class_io->value = 1;
        on_start(NULL, NULL, NULL);
        app_timer_add(app, auto_seconds * 1000, 0, auto_stop, NULL);
    }
    app_run(app);
    if (fd >= 0) {
        if (running)
            prof_stop(fd);
        close(fd);
    }
    app_destroy(app);
    prof_session_free(session);
    prof_resolver_free(res);
    free(buffer);
    free(boxes);
    free(break_rows);
    return 0;
}

/* Deterministic widget interaction checks used by the boot regression. */
#include "../tests/profiler_ui.h"
