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
#include <gui/pixel.h>
#include <gui/i18n.h>
#include <minios/proctab.h>

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
static struct widget *win, *tabs, *table, *threads_table, *filter;
/* The Resources tab: the total CPU share above a graph per processor, the
 * memory graph and the network graph (gui/widget.h, graph_new). */
static struct widget *cpu_total_label, *cpu_grid, *cpu_graphs[MAX_CPUS], *mem_graph, *net_graph;
static int ncpu_graphs;
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

/* The newest sample of each resource. The CPU and memory samples are in
 * tenths of a percent, the network samples in bytes per second. The
 * graphs retain the last HISTORY samples. */
static int ncpus;
static int cpu_now[MAX_CPUS], cpu_total_now;
static int mem_now, swap_now;
static long rx_now, tx_now;
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
 * last answer is retained because consecutive rows mostly share a user. */
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
    static struct proc_entry table[MAX_PROCS];
    int n = proc_table_read(table, MAX_PROCS);
    for (int k = 0; k < n; k++) {
        struct proc_entry *e = &table[k];
        struct proc_row r;
        r.pid = (int)e->pid;
        r.ppid = (int)e->ppid;
        r.pgid = (int)e->pgid;
        snprintf(r.state, sizeof r.state, "%s", e->state);
        r.ticks = (long)e->ticks;
        r.rss = (long)e->rss_kib;
        r.uid = e->uid;
        snprintf(r.name, sizeof r.name, "%s", e->name);
        user_name(r.uid, r.user, sizeof r.user);
        r.cpu = 0;
        for (int i = 0; i < nprev && elapsed > 0; i++)
            if (prev_rows[i].pid == r.pid) {
                /* TIME counts ticks of one millisecond. */
                r.cpu = (int)((r.ticks - prev_rows[i].ticks) * 1000 / elapsed);
                break;
            }
        rows[nrows++] = r;
    }
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


static long meminfo_value(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? strtol(p + strlen(key), NULL, 10) : 0;
}

/* read_resources takes one sample of every resource. */
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
        cpu_now[c] = have_prev_stat && c < n ? percent_of(busy, total) : 0;
        memcpy(prev_cpu[c], cur[c], sizeof cur[c]);
    }
    cpu_total_now = have_prev_stat ? percent_of(busy_all, total_all) : 0;

    text = read_file("/dev/meminfo", 2048);
    if (text) {
        mem_total_kb = meminfo_value(text, "MemTotal:");
        mem_used_kb = mem_total_kb - meminfo_value(text, "MemFree:");
        swap_total_kb = meminfo_value(text, "SwapTotal:");
        swap_used_kb = swap_total_kb - meminfo_value(text, "SwapFree:");
        cache_kb = meminfo_value(text, "FileMapped:") * 4;   /* FileMapped counts pages of 4 KiB. */
        free(text);
    }
    mem_now = percent_of((unsigned long)mem_used_kb, (unsigned long)mem_total_kb);
    swap_now = percent_of((unsigned long)swap_used_kb, (unsigned long)swap_total_kb);

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
    rx_now = rx_rate;
    tx_now = tx_rate;
    prev_rx = rx;
    prev_tx = tx;
    have_prev_stat = 1;
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
 * a process is its pid.  The selection therefore remains on the same process
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

/* The functions below update the Resources tab. */

static void rate_scale(long bytes, char *buf, size_t size)
{
    format_rate(buf, size, bytes);
}

/* Creates the graphs of processors that appeared since the last call.
 * The grid has up to four columns. */
static void add_cpu_graphs(void)
{
    int cols = ncpus < 4 ? ncpus : 4;
    for (; ncpu_graphs < ncpus && ncpu_graphs < MAX_CPUS; ncpu_graphs++) {
        struct widget *g = graph_new(cpu_grid, HISTORY);
        graph_add_series(g, "", 0, GRAPH_AREA);
        graph_set_scale(g, 1000, 0);
        widget_set_grid(g, ncpu_graphs / cols, ncpu_graphs % cols, 1, 1);
        widget_set_stretch(g, 1, 1);
        cpu_graphs[ncpu_graphs] = g;
    }
    for (int c = 0; c < cols; c++)
        grid_set_stretch(cpu_grid, -1, c, 1);
    for (int r = 0; r < (ncpus + cols - 1) / cols; r++)
        grid_set_stretch(cpu_grid, r, -1, 1);
    widget_relayout(cpu_grid);
}

static void update_graphs(void)
{
    char text[64], a[24], b[24];
    if (ncpu_graphs < ncpus)
        add_cpu_graphs();
    format_percent(a, sizeof a, cpu_total_now);
    widget_set_text(cpu_total_label, a);
    for (int c = 0; c < ncpu_graphs; c++) {
        long v = cpu_now[c];
        graph_push(cpu_graphs[c], &v);
        snprintf(text, sizeof text, _("CPU %d"), c);
        format_percent(a, sizeof a, cpu_now[c]);
        graph_set_title(cpu_graphs[c], text, a);
    }

    /* The memory graph fills the used memory and draws the used swap as a
     * line. */
    format_size(a, sizeof a, mem_used_kb);
    format_size(b, sizeof b, mem_total_kb);
    snprintf(text, sizeof text, _("Used %s of %s"), a, b);
    graph_set_label(mem_graph, 0, text);
    format_size(a, sizeof a, swap_used_kb);
    format_size(b, sizeof b, swap_total_kb);
    snprintf(text, sizeof text, _("Swap %s of %s"), a, b);
    graph_set_label(mem_graph, 1, swap_total_kb > 0 ? text : "");
    graph_set_style(mem_graph, 1, swap_total_kb > 0 ? GRAPH_LINE : GRAPH_TEXT);
    format_size(a, sizeof a, cache_kb);
    snprintf(text, sizeof text, _("File cache %s"), a);
    graph_set_label(mem_graph, 2, text);
    long mem[3] = { mem_now, swap_now, 0 };
    graph_push(mem_graph, mem);

    /* The network graph fills the receive rate and draws the transmit rate
     * as a line, both scaled to the largest rate in the history. */
    format_rate(a, sizeof a, rx_now);
    snprintf(text, sizeof text, _("Receive %s"), a);
    graph_set_label(net_graph, 0, text);
    format_rate(a, sizeof a, tx_now);
    snprintf(text, sizeof text, _("Transmit %s"), a);
    graph_set_label(net_graph, 1, text);
    long net[2] = { rx_now, tx_now };
    graph_push(net_graph, net);
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
    format_percent(a, sizeof a, cpu_total_now);
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
    update_graphs();
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
    struct widget *cpu_row = box_new(resources, 0);
    label_new(cpu_row, _("CPU"));
    widget_set_stretch(label_new(cpu_row, ""), 1, 0);
    cpu_total_label = label_new(cpu_row, "");
    cpu_grid = grid_new(resources);
    widget_set_stretch(cpu_grid, 1, 9);
    mem_graph = graph_new(resources, HISTORY);
    graph_set_title(mem_graph, _("Memory"), NULL);
    graph_add_series(mem_graph, "", 0, GRAPH_AREA);
    graph_add_series(mem_graph, "", 0, GRAPH_TEXT);
    graph_add_series(mem_graph, "", 0, GRAPH_TEXT);
    graph_set_scale(mem_graph, 1000, 0);
    widget_set_stretch(mem_graph, 1, 5);
    net_graph = graph_new(resources, HISTORY);
    graph_set_title(net_graph, _("Network"), NULL);
    graph_add_series(net_graph, "", 0, GRAPH_AREA);
    graph_add_series(net_graph, "", 0, GRAPH_LINE);
    graph_set_scale(net_graph, KIB, 1);
    graph_set_format(net_graph, rate_scale);
    widget_set_stretch(net_graph, 1, 5);

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
