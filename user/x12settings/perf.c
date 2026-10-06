/* The Performance page (X2 of docs/plan/x12settings.md): graphs of the
 * last minute and every frame statistic of X12.
 *
 * Once per second the page asks X12 for the frames after the newest one
 * it has seen (debug version 3), for the frame statistics and for the
 * basic counters. The frames of that second give the average and the
 * longest frame time, the average and the longest latency, and the
 * composed pixels. The growth of the wakeups counter gives the wakeups
 * of the second. Each graph shows 60 seconds. */
#include <stdio.h>
#include <string.h>
#include <gui/client.h>
#include <gui/model.h>
#include <minios/proctab.h>
#include "x12settings.h"
#include "debug-client.h"

#define SECONDS 60
#define MAX_VALUES 48
#define MAX_ROWS 64

static struct widget *frame_graph, *latency_graph, *pixel_graph, *rate_graph, *table, *period_label;

/* The frames that X12 reported for the current second. */
static struct {
    long frames, total_sum, total_max, latency_sum, latency_count, latency_max, pixels;
} second;
static uint32_t last_serial;
static int primed;              /* the first history only gives the newest serial */

/* The frame statistics, collected until frame_stats_done. */
static struct {
    char key[32];
    uint64_t value;
} values[MAX_VALUES], pending[MAX_VALUES];
static int nvalues, npending;
static uint64_t prev_wakeups;
static int have_wakeups;

/* The basic counters of the stats event. */
static uint32_t up_ms, clients, surfaces, frame_interval;

/* The rows of the table. */
static struct {
    char name[48], value[64];
} rows[MAX_ROWS];
static int nrows;

static const struct {
    const char *key, *name;
} names[] = {
    { "elapsed_us", "Period since the reset" },
    { "frames", "Frames" },
    { "rects", "Composed rectangles" },
    { "pixels", "Composed device pixels" },
    { "flushes", "Presents" },
    { "flush_rects", "Flushed rectangles" },
    { "flush_bytes", "Flushed bytes" },
    { "compose_us", "Compose time" },
    { "compose_max_us", "Longest compose time" },
    { "flush_us", "Flush time" },
    { "flush_max_us", "Longest flush time" },
    { "frame_max_us", "Longest frame" },
    { "frame_p50_us", "Median frame time" },
    { "frame_p95_us", "Frame time at 95 %" },
    { "frame_p99_us", "Frame time at 99 %" },
    { "damage_latency_us", "Damage latency, average" },
    { "damage_latency_max_us", "Damage latency, longest" },
    { "commit_latency_us", "Commit latency, average" },
    { "commit_latency_max_us", "Commit latency, longest" },
    { "input_latency_us", "Input latency, average" },
    { "input_latency_max_us", "Input latency, longest" },
    { "wakeups", "Wakeups of the main loop" },
    { "idle_timers", "Frames without damage" },
    { "cpu_us", "CPU time" },
    { "back_bytes", "Back buffer" },
    { "pool_bytes", "Mapped client buffers" },
    { "cursor_moves", "Device cursor moves" },
    { "cursor_us", "Device cursor move time" },
};

/* ---- formatting ---- */

static void format_us(long us, char *buf, size_t size)
{
    if (us >= 1000000)
        snprintf(buf, size, "%ld.%02ld s", us / 1000000, us / 10000 % 100);
    else
        snprintf(buf, size, "%ld.%02ld ms", us / 1000, us / 10 % 100);
}

static void format_bytes(uint64_t bytes, char *buf, size_t size)
{
    if (bytes >= 10u << 20)
        snprintf(buf, size, "%llu MiB", (unsigned long long)(bytes >> 20));
    else if (bytes >= 10u << 10)
        snprintf(buf, size, "%llu KiB", (unsigned long long)(bytes >> 10));
    else
        snprintf(buf, size, "%llu bytes", (unsigned long long)bytes);
}

static void format_pixels(long px, char *buf, size_t size)
{
    snprintf(buf, size, "%ld.%01ld Mpx", px / 1000000, px / 100000 % 10);
}

static void format_count(long n, char *buf, size_t size)
{
    snprintf(buf, size, "%ld", n);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static void format_value(const char *key, uint64_t v, char *buf, size_t size)
{
    if (ends_with(key, "_us"))
        format_us((long)v, buf, size);
    else if (ends_with(key, "_bytes"))
        format_bytes(v, buf, size);
    else
        snprintf(buf, size, "%llu", (unsigned long long)v);
}

/* ---- the table ---- */

static void add_row(const char *name, const char *value)
{
    if (nrows == MAX_ROWS)
        return;
    snprintf(rows[nrows].name, sizeof rows[nrows].name, "%s", name);
    snprintf(rows[nrows].value, sizeof rows[nrows].value, "%s", value);
    nrows++;
}

static void fill_table(void)
{
    char text[64];
    nrows = 0;
    snprintf(text, sizeof text, "%u:%02u:%02u", up_ms / 3600000, up_ms / 60000 % 60, up_ms / 1000 % 60);
    add_row("Uptime of X12", text);
    snprintf(text, sizeof text, "%u", clients);
    add_row("Clients", text);
    snprintf(text, sizeof text, "%u", surfaces);
    add_row("Surfaces", text);
    snprintf(text, sizeof text, "%u ms", frame_interval);
    add_row("Frame interval", text);
    struct gui_output_info info;
    if (gui_get_output(0, &info) == 0) {
        snprintf(text, sizeof text, "%dx%d at scale %d, %d Hz", info.width * info.scale, info.height * info.scale,
                 info.scale, info.refresh_hz);
        add_row("Display", text);
    }
    /* The resident pages of X12 include the scanout buffer and the
     * buffers of the clients, which it shares (docs/design/x12settings.md). */
    struct proc_entry procs[64];
    int n = proc_table_read(procs, 64);
    for (int i = 0; i < n; i++)
        if (strcmp(procs[i].name, "x12") == 0) {
            format_bytes((uint64_t)procs[i].rss_kib << 10, text, sizeof text);
            add_row("Resident memory of X12", text);
            break;
        }
    for (int i = 0; i < nvalues; i++) {
        const char *name = values[i].key;
        for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
            if (strcmp(names[k].key, values[i].key) == 0)
                name = names[k].name;
        format_value(values[i].key, values[i].value, text, sizeof text);
        add_row(name, text);
    }
    view_refresh(table);
}

static int m_rows(struct model *m, int parent) { return parent < 0 ? nrows : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 2; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    return col == 0 ? rows[row].name : rows[row].value;
}
static const char *m_header(struct model *m, int col) { return col == 0 ? "Statistic" : "Value"; }
static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL, NULL };

/* ---- events ---- */

void perf_stats(uint32_t uptime_ms, uint32_t nclients, uint32_t nsurfaces, uint32_t frame_ms)
{
    up_ms = uptime_ms;
    clients = nclients;
    surfaces = nsurfaces;
    frame_interval = frame_ms;
    if (table)
        fill_table();
}

void perf_frame(uint32_t serial, uint32_t total_us, uint32_t pixels, uint32_t latency_us)
{
    if (!primed)
        return;
    second.frames++;
    second.total_sum += total_us;
    if (total_us > second.total_max)
        second.total_max = total_us;
    if (latency_us) {
        second.latency_sum += latency_us;
        second.latency_count++;
        if (latency_us > second.latency_max)
            second.latency_max = latency_us;
    }
    second.pixels += pixels;
}

void perf_history_done(uint32_t last)
{
    last_serial = last;
    primed = 1;
}

void perf_frame_stat(const char *key, uint64_t value)
{
    if (npending == MAX_VALUES)
        return;
    snprintf(pending[npending].key, sizeof pending[npending].key, "%s", key);
    pending[npending].value = value;
    npending++;
}

static uint64_t value_of(const char *key)
{
    for (int i = 0; i < nvalues; i++)
        if (strcmp(values[i].key, key) == 0)
            return values[i].value;
    return 0;
}

/* The samples of the second that ended. */
static void push_second(void)
{
    char a[32], b[32], text[64];
    long avg = second.frames ? second.total_sum / second.frames : 0;
    long frame[2] = { avg, second.total_max };
    graph_push(frame_graph, frame);
    format_us(avg, a, sizeof a);
    format_us(second.total_max, b, sizeof b);
    snprintf(text, sizeof text, "Average %s", a);
    graph_set_label(frame_graph, 0, text);
    snprintf(text, sizeof text, "Longest %s", b);
    graph_set_label(frame_graph, 1, text);

    long lavg = second.latency_count ? second.latency_sum / second.latency_count : 0;
    long latency[2] = { lavg, second.latency_max };
    graph_push(latency_graph, latency);
    format_us(lavg, a, sizeof a);
    format_us(second.latency_max, b, sizeof b);
    snprintf(text, sizeof text, "Average %s", a);
    graph_set_label(latency_graph, 0, text);
    snprintf(text, sizeof text, "Longest %s", b);
    graph_set_label(latency_graph, 1, text);

    graph_push(pixel_graph, &second.pixels);
    format_pixels(second.pixels, a, sizeof a);
    snprintf(text, sizeof text, "%s per second", a);
    graph_set_label(pixel_graph, 0, text);

    uint64_t wakeups = value_of("wakeups");
    long woke = have_wakeups ? (long)(wakeups >= prev_wakeups ? wakeups - prev_wakeups : wakeups) : 0;
    prev_wakeups = wakeups;
    have_wakeups = 1;
    long rate[2] = { second.frames, woke };
    graph_push(rate_graph, rate);
    snprintf(text, sizeof text, "Frames %ld", second.frames);
    graph_set_label(rate_graph, 0, text);
    snprintf(text, sizeof text, "Wakeups %ld", woke);
    graph_set_label(rate_graph, 1, text);

    if (second.frames) {
        printf("x12settings: second of %ld frames, frame time %ld us average, %ld us longest, %ld pixels\n",
               second.frames, avg, second.total_max, second.pixels);
        fflush(stdout);
    }
    memset(&second, 0, sizeof second);
}

void perf_frame_stats_done(void)
{
    memcpy(values, pending, sizeof pending);
    nvalues = npending;
    npending = 0;
    if (!frame_graph)
        return;
    push_second();
    char text[64];
    format_us((long)value_of("elapsed_us"), text, sizeof text);
    char line[96];
    snprintf(line, sizeof line, "Statistics of the last %s", text);
    widget_set_text(period_label, line);
    fill_table();
}

void perf_tick(void)
{
    debug_get_frame_history(debug_proxy, last_serial);
    debug_get_frame_stats(debug_proxy);
    debug_get_stats(debug_proxy);
}

static int on_reset(struct widget *w, void *args, void *arg)
{
    debug_reset_frame_stats(debug_proxy);
    have_wakeups = 0;
    gui_flush();
    return 1;
}

/* ---- the page ---- */

static struct widget *new_graph(struct widget *grid, int row, int col, const char *title, long minimum,
                                void (*format)(long, char *, size_t), int two)
{
    struct widget *g = graph_new(grid, SECONDS);
    graph_set_title(g, title, NULL);
    graph_add_series(g, "", 0, GRAPH_AREA);
    if (two)
        graph_add_series(g, "", 0, GRAPH_LINE);
    graph_set_scale(g, minimum, 1);
    graph_set_format(g, format);
    widget_set_grid(g, row, col, 1, 1);
    widget_set_stretch(g, 1, 1);
    return g;
}

void perf_build(struct widget *tabs)
{
    struct widget *page = tabs_add(tabs, "Performance");
    struct widget *bar = toolbar_new(page);
    widget_connect(button_new(bar, "Reset statistics"), "clicked", on_reset, NULL);
    period_label = label_new(bar, "");
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 3);
    grid_set_stretch(grid, -1, 0, 1);
    grid_set_stretch(grid, -1, 1, 1);
    grid_set_stretch(grid, 0, -1, 1);
    grid_set_stretch(grid, 1, -1, 1);
    frame_graph = new_graph(grid, 0, 0, "Frame time", 1000, format_us, 1);
    latency_graph = new_graph(grid, 0, 1, "Latency", 1000, format_us, 1);
    pixel_graph = new_graph(grid, 1, 0, "Composed pixels", 1000000, format_pixels, 0);
    rate_graph = new_graph(grid, 1, 1, "Per second", 10, format_count, 1);
    table = table_new(page);
    view_set_model(table, &model);
    table_set_column_width(table, 0, 260);
    table_set_column_width(table, 1, 200);
    widget_set_stretch(table, 1, 2);
}
