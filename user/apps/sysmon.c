/* sysmon shows the processes and the resource usage of the system and
 * refreshes both once per second.
 *
 * The Processes tab lists the rows of /dev/proc in a table that can be
 * sorted by any column and filtered by name.  The CPU share of a process
 * is the growth of its TIME ticks between two refreshes.  The table below
 * it lists the threads of the selected process from /dev/threads.  A
 * selected process can be sent SIGTERM or SIGKILL or opened in
 * profiler(1).
 *
 * The Resources tab draws the last 60 seconds of the CPU usage of each
 * processor from /dev/cpustat, of the memory and swap usage from
 * /dev/meminfo and of the receive and transmit rates of the network
 * interfaces from /dev/net. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <langinfo.h>
#include <pwd.h>
#include <gui/app.h>
#include <gui/model.h>
#include <gui/i18n.h>

#define MAX_PROCS 64
#define MAX_THREADS 128
#define MAX_CPUS 16
#define HISTORY 60
#define KIB 1024L

struct proc_row {
    int pid, ppid, pgid;
    long ticks, rss;
    int cpu;                        /* cpu is the CPU share in tenths of a percent. */
    unsigned uid;
    char state[12], name[32], user[32];
};

struct thread_row {
    int tid, cpu;
    long wait_ms;                   /* wait_ms is the duration of a bounded wait or -1. */
    char name[32], state[12], queue[32];
};

static struct app *app;
static struct widget *win, *tabs, *table, *threads_table, *filter, *graphs;
static struct widget *st_procs, *st_cpu, *st_mem, *st_up;
static struct widget *context_menu;

static struct proc_row rows[MAX_PROCS], prev_rows[MAX_PROCS];
static int nrows, nprev;
static int order[MAX_PROCS], nview;
static int sort_col = 1, sort_desc;
static long prev_ms;
static int selected_pid = -1;

static struct thread_row threads[MAX_THREADS];
static int nthreads;

/* Each history array contains up to HISTORY samples, oldest first.  The
 * CPU and memory samples are in tenths of a percent, the network samples
 * in bytes per second. */
static int ncpus;
static int cpu_hist[MAX_CPUS][HISTORY], cpu_total_hist[HISTORY];
static int mem_hist[HISTORY], swap_hist[HISTORY];
static long rx_hist[HISTORY], tx_hist[HISTORY];
static int nhist;
static unsigned long prev_cpu[MAX_CPUS][3];
static unsigned long prev_rx, prev_tx;
static int have_prev_stat;
static long mem_total_kb, mem_used_kb, swap_total_kb, swap_used_kb, cache_kb;

/* The functions below read and parse the files of /dev. */

static char *read_file(const char *path, size_t limit)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char *text = malloc(limit);
    size_t n = 0;
    if (text) {
        size_t r;
        while (n < limit - 1 && (r = fread(text + n, 1, limit - 1 - n, f)) > 0)
            n += r;
        text[n] = '\0';
    }
    fclose(f);
    return text;
}

/* copy_word copies the next word of *p into out and advances *p past it. */
static void copy_word(char **p, char *out, size_t size)
{
    char *s = *p;
    while (*s == ' ')
        s++;
    size_t k = 0;
    while (*s && *s != ' ' && *s != '\n') {
        if (k + 1 < size)
            out[k++] = *s;
        s++;
    }
    out[k] = '\0';
    *p = s;
}

static struct proc_row *find_row(int pid)
{
    for (int i = 0; i < nrows; i++)
        if (rows[i].pid == pid)
            return &rows[i];
    return NULL;
}

/* The account name of uid, or the number when /etc/passwd has none. The
 * last answer is kept because consecutive rows mostly share a user. */
static void user_name(unsigned uid, char *out, size_t size)
{
    static unsigned last_uid = (unsigned)-1;
    static char last_name[32];
    if (uid != last_uid) {
        struct passwd *pw = getpwuid(uid);
        if (pw)
            snprintf(last_name, sizeof last_name, "%s", pw->pw_name);
        else
            snprintf(last_name, sizeof last_name, "%u", uid);
        last_uid = uid;
    }
    snprintf(out, size, "%s", last_name);
}

static void read_procs(long elapsed)
{
    memcpy(prev_rows, rows, sizeof rows);
    nprev = nrows;
    nrows = 0;
    char *text = read_file("/dev/proc", 8192);
    if (!text)
        return;
    for (char *line = strtok(text, "\n"); line && nrows < MAX_PROCS; line = strtok(NULL, "\n")) {
        if (line[0] == 'P' || strstr(line, "PID"))
            continue;
        struct proc_row r;
        char *end;
        r.pid = (int)strtol(line, &end, 10);
        if (end == line)
            continue;
        r.ppid = (int)strtol(end, &end, 10);
        r.pgid = (int)strtol(end, &end, 10);
        copy_word(&end, r.state, sizeof r.state);
        r.ticks = strtol(end, &end, 10);
        r.rss = strtol(end, &end, 10);
        r.uid = (unsigned)strtoul(end, &end, 10);
        copy_word(&end, r.name, sizeof r.name);
        user_name(r.uid, r.user, sizeof r.user);
        if (!r.state[0] || !r.name[0])
            continue;
        r.cpu = 0;
        for (int i = 0; i < nprev && elapsed > 0; i++)
            if (prev_rows[i].pid == r.pid) {
                /* TIME counts ticks of one millisecond. */
                r.cpu = (int)((r.ticks - prev_rows[i].ticks) * 1000 / elapsed);
                break;
            }
        rows[nrows++] = r;
    }
    free(text);
}

/* read_threads parses the lines of /dev/threads that belong to pid.  The
 * indented lines of kernel frames are skipped. */
static void read_threads(int pid)
{
    nthreads = 0;
    if (pid < 0)
        return;
    char *text = read_file("/dev/threads", 65536);
    if (!text)
        return;
    for (char *line = strtok(text, "\n"); line && nthreads < MAX_THREADS; line = strtok(NULL, "\n")) {
        if (line[0] == ' ')
            continue;
        char *p;
        if ((int)strtol(line, &p, 10) != pid || p == line)
            continue;
        struct thread_row t;
        char word[32];
        t.tid = (int)strtol(p, &p, 10);
        copy_word(&p, word, sizeof word);             /* The process name is skipped. */
        copy_word(&p, t.name, sizeof t.name);
        copy_word(&p, t.state, sizeof t.state);
        copy_word(&p, word, sizeof word);             /* The word "cpu" precedes the number. */
        t.cpu = (int)strtol(p, &p, 10);
        copy_word(&p, word, sizeof word);             /* The word "wq" precedes the queue. */
        copy_word(&p, t.queue, sizeof t.queue);
        t.wait_ms = -1;
        char *bounded = strstr(p, "bounded wait ");
        if (bounded)
            t.wait_ms = strtol(bounded + 13, NULL, 10);
        threads[nthreads++] = t;
    }
    free(text);
}

static int percent_of(unsigned long part, unsigned long whole)
{
    return whole ? (int)(part * 1000 / whole) : 0;
}

static void push_sample(int *hist, int value)
{
    if (nhist == HISTORY)
        memmove(hist, hist + 1, (HISTORY - 1) * sizeof *hist);
    hist[nhist == HISTORY ? HISTORY - 1 : nhist] = value;
}

static void push_sample_long(long *hist, long value)
{
    if (nhist == HISTORY)
        memmove(hist, hist + 1, (HISTORY - 1) * sizeof *hist);
    hist[nhist == HISTORY ? HISTORY - 1 : nhist] = value;
}

static long meminfo_value(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? strtol(p + strlen(key), NULL, 10) : 0;
}

/* read_resources appends one sample to every history array. */
static void read_resources(long elapsed)
{
    unsigned long cur[MAX_CPUS][3] = { { 0 } };
    int n = 0;
    char *text = read_file("/dev/cpustat", 4096);
    if (text) {
        for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
            char *p;
            long id = strtol(line, &p, 10);
            if (p == line || id < 0 || id >= MAX_CPUS)
                continue;
            for (int k = 0; k < 3; k++)
                cur[id][k] = strtoul(p, &p, 10);
            if (id + 1 > n)
                n = (int)id + 1;
        }
        free(text);
    }
    ncpus = n;
    unsigned long busy_all = 0, total_all = 0;
    for (int c = 0; c < MAX_CPUS; c++) {
        unsigned long busy = cur[c][0] + cur[c][1] - prev_cpu[c][0] - prev_cpu[c][1];
        unsigned long total = busy + cur[c][2] - prev_cpu[c][2];
        if (c < n) {
            busy_all += busy;
            total_all += total;
        }
        push_sample(cpu_hist[c], have_prev_stat && c < n ? percent_of(busy, total) : 0);
        memcpy(prev_cpu[c], cur[c], sizeof cur[c]);
    }
    push_sample(cpu_total_hist, have_prev_stat ? percent_of(busy_all, total_all) : 0);

    text = read_file("/dev/meminfo", 2048);
    if (text) {
        mem_total_kb = meminfo_value(text, "MemTotal:");
        mem_used_kb = mem_total_kb - meminfo_value(text, "MemFree:");
        swap_total_kb = meminfo_value(text, "SwapTotal:");
        swap_used_kb = swap_total_kb - meminfo_value(text, "SwapFree:");
        cache_kb = meminfo_value(text, "FileMapped:") * 4;   /* FileMapped counts pages of 4 KiB. */
        free(text);
    }
    push_sample(mem_hist, percent_of((unsigned long)mem_used_kb, (unsigned long)mem_total_kb));
    push_sample(swap_hist, percent_of((unsigned long)swap_used_kb, (unsigned long)swap_total_kb));

    /* The interface lines of /dev/net have the form
     * "INDEX NAME STATE mtu MTU rx PACKETS/BYTES drop N tx PACKETS/BYTES ...".
     * The loopback interface is not counted. */
    unsigned long rx = 0, tx = 0;
    text = read_file("/dev/net", 8192);
    if (text) {
        for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
            char *p, name[16];
            strtol(line, &p, 10);
            if (p == line || !strstr(p, " mtu "))
                continue;
            copy_word(&p, name, sizeof name);
            char *r = strstr(p, " rx "), *t = strstr(p, " tx ");
            if (!r || !t || strcmp(name, "lo") == 0)
                continue;
            char *slash = strchr(r, '/');
            if (slash)
                rx += strtoul(slash + 1, NULL, 10);
            slash = strchr(t, '/');
            if (slash)
                tx += strtoul(slash + 1, NULL, 10);
        }
        free(text);
    }
    long rx_rate = have_prev_stat && elapsed > 0 ? (long)((rx - prev_rx) * 1000 / (unsigned long)elapsed) : 0;
    long tx_rate = have_prev_stat && elapsed > 0 ? (long)((tx - prev_tx) * 1000 / (unsigned long)elapsed) : 0;
    push_sample_long(rx_hist, rx_rate);
    push_sample_long(tx_hist, tx_rate);
    prev_rx = rx;
    prev_tx = tx;
    have_prev_stat = 1;
    if (nhist < HISTORY)
        nhist++;
}

/* The functions below format values for the tables and the graphs. */

/* The sizes and percentages with one decimal place use the radix
 * character of the locale. */
static void format_size(char *buf, size_t size, long kib)
{
    const char *radix = nl_langinfo(RADIXCHAR);
    if (kib >= 10 * KIB * KIB)
        snprintf(buf, size, _("%ld GiB"), kib / (KIB * KIB));
    else if (kib >= KIB * KIB)
        snprintf(buf, size, _("%ld%s%ld GiB"), kib / (KIB * KIB), radix, kib % (KIB * KIB) * 10 / (KIB * KIB));
    else if (kib >= 10 * KIB)
        snprintf(buf, size, _("%ld MiB"), kib / KIB);
    else if (kib >= KIB)
        snprintf(buf, size, _("%ld%s%ld MiB"), kib / KIB, radix, kib % KIB * 10 / KIB);
    else
        snprintf(buf, size, _("%ld KiB"), kib);
}

static void format_rate(char *buf, size_t size, long bytes)
{
    if (bytes < KIB)
        snprintf(buf, size, _("%ld B/s"), bytes);
    else {
        char amount[32];
        format_size(amount, sizeof amount, bytes / KIB);
        snprintf(buf, size, _("%s/s"), amount);
    }
}

static void format_percent(char *buf, size_t size, int tenths)
{
    snprintf(buf, size, _("%d%s%d %%"), tenths / 10, nl_langinfo(RADIXCHAR), tenths % 10);
}

/* The functions below implement the process table model.  The row id of
 * a process is its pid.  The selection therefore stays on the same process
 * when the order of the rows changes. */

static int compare_rows(const void *a, const void *b)
{
    const struct proc_row *x = &rows[*(const int *)a], *y = &rows[*(const int *)b];
    long d = 0;
    switch (sort_col) {
    case 0: d = strcmp(x->name, y->name); break;
    case 1: d = strcmp(x->user, y->user); break;
    case 2: d = x->pid - y->pid; break;
    case 3: d = x->ppid - y->ppid; break;
    case 4: d = strcmp(x->state, y->state); break;
    case 5: d = x->cpu - y->cpu; break;
    case 6: d = x->ticks - y->ticks; break;
    default: d = x->rss - y->rss; break;
    }
    if (d == 0)
        d = x->pid - y->pid;
    if (sort_desc)
        d = -d;
    return d < 0 ? -1 : d > 0;
}

static void build_view(void)
{
    const char *needle = filter ? widget_text(filter) : "";
    nview = 0;
    for (int i = 0; i < nrows; i++)
        if (!needle || !needle[0] || strstr(rows[i].name, needle))
            order[nview++] = i;
    qsort(order, (size_t)nview, sizeof order[0], compare_rows);
}

static int m_rows(struct model *m, int parent) { return parent < 0 ? nview : 0; }
static int m_child(struct model *m, int parent, int index) { return rows[order[index]].pid; }
static int m_columns(struct model *m) { return 8; }

static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct proc_row *r = find_row(row);
    if (!r)
        return "";
    switch (col) {
    case 0: return r->name;
    case 1: return r->user;
    case 2: snprintf(buf, size, "%d", r->pid); return buf;
    case 3: snprintf(buf, size, "%d", r->ppid); return buf;
    case 4: return r->state;
    case 5: format_percent(buf, size, r->cpu); return buf;
    case 6: snprintf(buf, size, "%ld:%02ld", r->ticks / 60000, r->ticks / 1000 % 60); return buf;
    default: format_size(buf, size, r->rss); return buf;
    }
}

static const char *m_header(struct model *m, int col)
{
    static const char *const names[] = { N_("Name"), N_("User"), N_("PID"), N_("Parent"), N_("State"), N_("CPU"),
                                         N_("CPU time"), N_("Memory") };
    return _(names[col]);
}

static void m_sort(struct model *m, int col, int descending)
{
    sort_col = col;
    sort_desc = descending;
    build_view();
}

static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, m_sort, NULL, NULL };

static int t_rows(struct model *m, int parent) { return parent < 0 ? nthreads : 0; }
static int t_child(struct model *m, int parent, int index) { return index; }
static int t_columns(struct model *m) { return 6; }

static const char *t_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct thread_row *t = &threads[row];
    switch (col) {
    case 0: snprintf(buf, size, "%d", t->tid); return buf;
    case 1: return t->name;
    case 2: return t->state;
    case 3: snprintf(buf, size, "%d", t->cpu); return buf;
    case 4: return strcmp(t->queue, "-") == 0 ? "" : t->queue;
    default:
        if (t->wait_ms < 0)
            return "";
        snprintf(buf, size, _("%ld ms"), t->wait_ms);
        return buf;
    }
}

static const char *t_header(struct model *m, int col)
{
    static const char *const names[] = { N_("TID"), N_("Name"), N_("State"), N_("CPU"), N_("Wait queue"),
                                         N_("Bounded wait") };
    return _(names[col]);
}

static struct model thread_model = { t_rows, t_child, t_columns, t_cell, t_header, NULL, NULL, NULL };

/* The functions below draw the Resources tab. */

static uint32_t blend(uint32_t a, uint32_t b, int alpha)
{
    uint32_t r = (((a >> 16) & 255) * (uint32_t)alpha + ((b >> 16) & 255) * (uint32_t)(256 - alpha)) >> 8;
    uint32_t g = (((a >> 8) & 255) * (uint32_t)alpha + ((b >> 8) & 255) * (uint32_t)(256 - alpha)) >> 8;
    uint32_t bl = ((a & 255) * (uint32_t)alpha + (b & 255) * (uint32_t)(256 - alpha)) >> 8;
    return r << 16 | g << 8 | bl;
}

/* draw_series draws n samples of values scaled to max into the graph
 * rectangle.  The newest sample is at the right edge, and one sample
 * covers w / (HISTORY - 1) pixels.  The area under the line is filled when
 * fill is not 0. */
static void draw_series(struct painter *p, int x, int y, int w, int h, const long *values, int n, long max,
                        uint32_t line, uint32_t fill)
{
    if (n < 1 || max <= 0 || w < 2 || h < 2)
        return;
    int px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        int sx = x + w - 1 - (int)((long)(n - 1 - i) * (w - 1) / (HISTORY - 1));
        long v = values[i] > max ? max : values[i] < 0 ? 0 : values[i];
        int sy = y + h - 1 - (int)(v * (h - 1) / max);
        if (i > 0) {
            if (fill)
                for (int cx = px; cx <= sx; cx++) {
                    int cy = sx == px ? sy : py + (sy - py) * (cx - px) / (sx - px);
                    painter_fill(p, cx, cy, 1, y + h - cy, fill);
                }
            painter_line(p, px, py, sx, sy, line);
        }
        px = sx;
        py = sy;
    }
}

static void draw_frame(struct painter *p, int x, int y, int w, int h)
{
    const struct theme *t = p->theme;
    painter_fill(p, x, y, w, h, t->color[TC_FIELD]);
    uint32_t grid = blend(t->color[TC_BORDER], t->color[TC_FIELD], 80);
    for (int k = 1; k < 4; k++)
        painter_fill(p, x + 1, y + h * k / 4, w - 2, 1, grid);
    painter_frame(p, x, y, w, h, t->color[TC_BORDER]);
}

/* draw_heading draws a title on the left and a value on the right of a
 * text row and returns the height of the row. */
static int draw_heading(struct painter *p, int x, int y, int w, const char *title, const char *value)
{
    const struct theme *t = p->theme;
    painter_text(p, x, y, title, t->color[TC_TEXT]);
    if (value)
        painter_text(p, x + w - painter_text_width(p, value, -1), y, value, t->color[TC_TEXT_DISABLED]);
    return painter_text_height(p) + 4;
}

/* A legend item is a text with a colour swatch in front of it, or
 * without a swatch when swatch is 0. */
struct legend_item {
    const char *text;
    uint32_t swatch;
};

/* draw_legend draws the items right-aligned at the end of a text row that
 * ends at x + w. */
static void draw_legend(struct painter *p, int x, int y, int w, const struct legend_item *items, int n)
{
    const struct theme *t = p->theme;
    int th = painter_text_height(p), box = th / 2, right = x + w;
    for (int i = n - 1; i >= 0; i--) {
        int tw = painter_text_width(p, items[i].text, -1);
        right -= tw;
        painter_text(p, right, y, items[i].text, t->color[TC_TEXT_DISABLED]);
        if (items[i].swatch) {
            right -= box + 5;
            painter_fill(p, right, y + (th - box) / 2, box, box, items[i].swatch);
        }
        right -= 16;
    }
}

static void to_long(const int *in, long *out, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = in[i];
}

/* nice_max rounds a rate up to 1, 2 or 5 times a power of ten, with a
 * minimum of 1 KiB/s. */
static long nice_max(long v)
{
    long step = 1;
    if (v < KIB)
        return KIB;
    while (step * 10 <= v)
        step *= 10;
    if (v <= step)
        return step;
    if (v <= 2 * step)
        return 2 * step;
    if (v <= 5 * step)
        return 5 * step;
    return 10 * step;
}

static int on_paint_graphs(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = p->theme;
    uint32_t accent = t->color[TC_ACCENT], area = blend(accent, t->color[TC_FIELD], 72);
    uint32_t second = t->color[TC_TEXT];
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    int pad = 8, gap = 12, x = pad, width = w->w - 2 * pad;
    int th = painter_text_height(p) + 4;
    if (width < 40 || w->h < 6 * th)
        return 1;
    long buf[HISTORY], buf2[HISTORY];
    char a[48], b[48], text[128];

    /* The CPU section is 45 percent of the height and contains one graph
     * per processor, at most four in a row. */
    int y = pad;
    int cpu_h = (w->h - 2 * pad - 2 * gap) * 45 / 100;
    int mem_h = (w->h - 2 * pad - 2 * gap - cpu_h) / 2;
    format_percent(a, sizeof a, nhist ? cpu_total_hist[nhist - 1] : 0);
    y += draw_heading(p, x, y, width, _("CPU"), a);
    int n = ncpus > 0 ? ncpus : 1;
    int cols = n < 4 ? n : 4, lines = (n + cols - 1) / cols;
    int cell_w = (width - (cols - 1) * pad) / cols;
    int cell_h = (cpu_h - th - (lines - 1) * pad) / lines;
    for (int c = 0; c < n; c++) {
        int cx = x + (c % cols) * (cell_w + pad), cy = y + (c / cols) * (cell_h + pad);
        snprintf(text, sizeof text, _("CPU %d"), c);
        format_percent(a, sizeof a, nhist ? cpu_hist[c][nhist - 1] : 0);
        int hh = cell_h > 3 * th ? draw_heading(p, cx, cy, cell_w, text, a) : 0;
        draw_frame(p, cx, cy + hh, cell_w, cell_h - hh);
        to_long(cpu_hist[c], buf, nhist);
        draw_series(p, cx + 1, cy + hh + 1, cell_w - 2, cell_h - hh - 2, buf, nhist, 1000, accent, area);
    }
    y += cpu_h - th + gap;

    /* The memory graph fills the used memory and draws the used swap as a
     * line. */
    char used[64], swap[64], cache[48];
    format_size(a, sizeof a, mem_used_kb);
    format_size(b, sizeof b, mem_total_kb);
    snprintf(used, sizeof used, _("Used %s of %s"), a, b);
    format_size(a, sizeof a, swap_used_kb);
    format_size(b, sizeof b, swap_total_kb);
    snprintf(swap, sizeof swap, _("Swap %s of %s"), a, b);
    format_size(a, sizeof a, cache_kb);
    snprintf(cache, sizeof cache, _("File cache %s"), a);
    struct legend_item mem_items[] = { { used, accent }, { swap, second }, { cache, 0 } };
    int hh = draw_heading(p, x, y, width, _("Memory"), NULL);
    if (swap_total_kb > 0)
        draw_legend(p, x, y, width, mem_items, 3);
    else {
        mem_items[1] = mem_items[2];
        draw_legend(p, x, y, width, mem_items, 2);
    }
    draw_frame(p, x, y + hh, width, mem_h - hh);
    to_long(mem_hist, buf, nhist);
    draw_series(p, x + 1, y + hh + 1, width - 2, mem_h - hh - 2, buf, nhist, 1000, accent, area);
    if (swap_total_kb > 0) {
        to_long(swap_hist, buf2, nhist);
        draw_series(p, x + 1, y + hh + 1, width - 2, mem_h - hh - 2, buf2, nhist, 1000, second, 0);
    }
    y += mem_h + gap;

    /* The network graph fills the receive rate and draws the transmit rate
     * as a line, both scaled to the largest rate in the history. */
    long peak = 0;
    for (int i = 0; i < nhist; i++) {
        if (rx_hist[i] > peak) peak = rx_hist[i];
        if (tx_hist[i] > peak) peak = tx_hist[i];
    }
    long max = nice_max(peak);
    char receive[48], transmit[48];
    format_rate(a, sizeof a, nhist ? rx_hist[nhist - 1] : 0);
    snprintf(receive, sizeof receive, _("Receive %s"), a);
    format_rate(a, sizeof a, nhist ? tx_hist[nhist - 1] : 0);
    snprintf(transmit, sizeof transmit, _("Transmit %s"), a);
    struct legend_item net_items[] = { { receive, accent }, { transmit, second } };
    hh = draw_heading(p, x, y, width, _("Network"), NULL);
    draw_legend(p, x, y, width, net_items, 2);
    int net_h = w->h - pad - y;
    draw_frame(p, x, y + hh, width, net_h - hh);
    draw_series(p, x + 1, y + hh + 1, width - 2, net_h - hh - 2, rx_hist, nhist, max, accent, area);
    draw_series(p, x + 1, y + hh + 1, width - 2, net_h - hh - 2, tx_hist, nhist, max, second, 0);
    format_rate(a, sizeof a, max);
    painter_text(p, x + 4, y + hh + 2, a, t->color[TC_TEXT_DISABLED]);
    return 1;
}

/* The functions below refresh the window. */

static void update_threads(void)
{
    if (tabs->value == 0) {
        read_threads(selected_pid);
        view_refresh(threads_table);
    }
}

static void update_status(void)
{
    char text[64], a[24], b[24];
    snprintf(text, sizeof text, ngettext("%d process", "%d processes", (unsigned long)nrows), nrows);
    widget_set_text(st_procs, text);
    format_percent(a, sizeof a, nhist ? cpu_total_hist[nhist - 1] : 0);
    snprintf(text, sizeof text, _("CPU %s"), a);
    widget_set_text(st_cpu, text);
    format_size(a, sizeof a, mem_used_kb);
    format_size(b, sizeof b, mem_total_kb);
    snprintf(text, sizeof text, _("Memory %s of %s"), a, b);
    widget_set_text(st_mem, text);
    long up = uptime_ms() / 1000;
    snprintf(text, sizeof text, _("Up %ld:%02ld:%02ld"), up / 3600, up / 60 % 60, up % 60);
    widget_set_text(st_up, text);
}

static void refresh_table(void)
{
    build_view();
    view_refresh(table);
    if (selected_pid >= 0) {
        int found = 0;
        for (int i = 0; i < nview; i++)
            found |= rows[order[i]].pid == selected_pid;
        if (!found)
            selected_pid = -1;
    }
    table->value = selected_pid;
}

static void refresh(void)
{
    long now = uptime_ms();
    long elapsed = prev_ms ? now - prev_ms : 0;
    prev_ms = now;
    read_procs(elapsed);
    read_resources(elapsed);
    refresh_table();
    update_threads();
    update_status();
    widget_invalidate(graphs);
}

static void on_tick(void *arg)
{
    refresh();
}

/* The functions below implement the commands. */

static int on_select(struct widget *w, void *args, void *arg)
{
    selected_pid = w->value;
    update_threads();
    return 1;
}

static int on_filter(struct widget *w, void *args, void *arg)
{
    refresh_table();
    update_threads();
    return 1;
}

static int on_tab_changed(struct widget *w, void *args, void *arg)
{
    update_threads();
    widget_invalidate(graphs);
    return 1;
}

static int on_signal(struct widget *w, void *args, void *arg)
{
    int sig = (int)(long)arg;
    if (selected_pid <= 0 || !find_row(selected_pid))
        return 1;
    printf("sysmon: signal %d to pid %d\n", sig, selected_pid);
    fflush(stdout);
    if (kill(selected_pid, sig) < 0) {
        const char *buttons[] = { _("Close") };
        char text[160];
        snprintf(text, sizeof text, _("The signal could not be sent to process %d."), selected_pid);
        app_dialog(app, _("Error"), text, buttons, 1);
    }
    refresh();
    return 1;
}

/* The profiler is a separate program.  The intermediate child exits at
 * once and is reaped here, and the profiler process is then a child of
 * init. */
static int on_profile(struct widget *w, void *args, void *arg)
{
    char pid[16] = "";
    if (selected_pid > 0)
        snprintf(pid, sizeof pid, "%d", selected_pid);
    pid_t child = fork();
    if (child == 0) {
        if (fork() == 0) {
            if (pid[0])
                execl("/bin/profiler", "profiler", "-p", pid, (char *)NULL);
            else
                execl("/bin/profiler", "profiler", (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (child > 0)
        waitpid(child, NULL, 0);
    return 1;
}

static int on_context(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    int x, y;
    selected_pid = w->value;
    update_threads();
    widget_abs(w, &x, &y);
    menu_popup(context_menu, x + c->x, y + c->y);
    return 1;
}

static int on_show_tab(struct widget *w, void *args, void *arg)
{
    tabs_select(tabs, (int)(long)arg);
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg)
{
    app_quit(app, 0);
    return 1;
}

/* add_process_items adds the process commands to a menu.  The items of
 * the menu bar receive the accelerators Ctrl+E and Ctrl+K. */
static void add_process_items(struct widget *menu, int accel)
{
    struct widget *m = menu_add(menu, _("End process"), "stop");
    widget_connect(m, "clicked", on_signal, (void *)SIGTERM);
    if (accel)
        widget_set_accel(m, KEY_E, WMOD_CTRL);
    m = menu_add(menu, _("Kill process"), "kill");
    widget_connect(m, "clicked", on_signal, (void *)SIGKILL);
    if (accel)
        widget_set_accel(m, KEY_K, WMOD_CTRL);
    menu_add_separator(menu);
    widget_connect(menu_add(menu, _("Profile"), "profile"), "clicked", on_profile, NULL);
}

int main(void)
{
    app = app_create();
    if (!app)
        return 1;
    textdomain("sysmon");
    win = app_window(app, 760, 540, _("System monitor"));
    if (!win)
        return 1;
    struct widget *mb = menubar_new(win);
    struct widget *file = menu_new(mb, _("File"));
    struct widget *m = menu_add(file, _("Quit"), "quit");
    widget_connect(m, "clicked", on_quit, NULL);
    widget_set_accel(m, KEY_Q, WMOD_CTRL);
    struct widget *process = menu_new(mb, _("Process"));
    add_process_items(process, 1);
    struct widget *view = menu_new(mb, _("View"));
    m = menu_add(view, _("Processes"), NULL);
    widget_connect(m, "clicked", on_show_tab, (void *)0L);
    widget_set_accel(m, KEY_1, WMOD_CTRL);
    m = menu_add(view, _("Resources"), NULL);
    widget_connect(m, "clicked", on_show_tab, (void *)1L);
    widget_set_accel(m, KEY_2, WMOD_CTRL);

    tabs = tabs_new(win);
    widget_set_stretch(tabs, 1, 1);
    widget_connect(tabs, "changed", on_tab_changed, NULL);
    struct widget *procs = tabs_add(tabs, _("Processes"));
    struct widget *tools = toolbar_new(procs);
    widget_connect(toolbar_add(tools, "stop", _("End process")), "clicked", on_signal, (void *)SIGTERM);
    widget_connect(toolbar_add(tools, "kill", _("Kill process")), "clicked", on_signal, (void *)SIGKILL);
    widget_connect(toolbar_add(tools, "profile", _("Profile")), "clicked", on_profile, NULL);
    filter = textfield_new(tools, "");
    widget_set_hint(filter, 180, 0);
    widget_set_max(filter, 180, 0);
    widget_set_tip(filter, _("Filter by name"));
    widget_connect(filter, "changed", on_filter, NULL);

    struct widget *split = splitpane_new(procs, 1);
    widget_set_stretch(split, 1, 1);
    table = table_new(split);
    view_set_model(table, &model);
    static const int widths[] = { 170, 80, 60, 60, 80, 70, 80, 90 };
    for (int c = 0; c < 8; c++)
        table_set_column_width(table, c, widths[c]);
    widget_connect(table, "selected", on_select, NULL);
    widget_connect(table, "context", on_context, NULL);
    threads_table = table_new(split);
    view_set_model(threads_table, &thread_model);
    static const int thread_widths[] = { 60, 150, 90, 50, 150, 110 };
    for (int c = 0; c < 6; c++)
        table_set_column_width(threads_table, c, thread_widths[c]);
    splitpane_set_position(split, 300);

    struct widget *resources = tabs_add(tabs, _("Resources"));
    graphs = canvas_new(resources);
    widget_set_stretch(graphs, 1, 1);
    widget_connect(graphs, "paint", on_paint_graphs, NULL);

    context_menu = popupmenu_new(win);
    add_process_items(context_menu, 0);

    struct widget *sb = statusbar_new(win);
    st_procs = statusbar_add(sb, 1);
    st_cpu = statusbar_add(sb, 0);
    widget_set_min(st_cpu, 90, 0);
    st_mem = statusbar_add(sb, 0);
    widget_set_min(st_mem, 190, 0);
    st_up = statusbar_add(sb, 0);
    widget_set_min(st_up, 90, 0);

    refresh();
    app_timer_add(app, 1000, 1, on_tick, NULL);
    widget_focus(table);
    printf("sysmon: %d cpus, %d processes\n", ncpus, nrows);
    fflush(stdout);
    app_run(app);
    app_destroy(app);
    return 0;
}
