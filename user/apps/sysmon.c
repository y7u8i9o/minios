/* sysmon: processes, memory and a sampling profiler, refreshed every
 * second. The process table comes from /dev/proc (with the CPU share
 * computed from the TIME column between refreshes and the resident size
 * from RSS), memory from /dev/meminfo; a selected process can be sent
 * SIGTERM or SIGKILL. The Profile tab samples the selected process (or
 * every process) through /dev/profile and lists the hottest symbols. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <gui/app.h>
#include <gui/model.h>
#include <minios/profile.h>

#define MAX_PROCS 64
#define MAX_SYMTABS 16
#define PROF_ROWS 200

struct proc_row { int pid, ppid, pgid; long ticks, rss; int cpu_pct; char state[12], name[32]; };

static struct app *app;
static struct widget *table, *mem_label, *status;
static struct proc_row rows[MAX_PROCS], prev_rows[MAX_PROCS];
static int nrows, nprev;
static long prev_ms;

/* ---- process table model ---- */

static int m_rows(struct model *m, int parent) { return parent < 0 ? nrows : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 7; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct proc_row *r = &rows[row];
    switch (col) {
    case 0: snprintf(buf, size, "%d", r->pid); return buf;
    case 1: snprintf(buf, size, "%d", r->ppid); return buf;
    case 2: snprintf(buf, size, "%d", r->pgid); return buf;
    case 3: return r->state;
    case 4: snprintf(buf, size, "%d.%d", r->cpu_pct / 10, r->cpu_pct % 10); return buf;
    case 5: snprintf(buf, size, "%ld", r->rss); return buf;
    default: return r->name;
    }
}
static const char *m_header(struct model *m, int col)
{
    static const char *const names[] = { "PID", "PPID", "PGID", "State", "CPU %", "RSS KiB", "Name" };
    return names[col];
}
static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL, NULL };

/* ---- profile ---- */

static struct widget *prof_table, *prof_status, *kernel_box, *start_btn, *stop_btn;
static int prof_fd = -1;
static struct watch *prof_watch;
static struct prof_hist hist;
static struct prof_symtab *ksyms;
static struct { char name[32]; struct prof_symtab *syms; } symtabs[MAX_SYMTABS];
static int nsymtabs;
static unsigned kernel_samples, user_samples;
static int prof_pid;
static struct prof_bucket shown[PROF_ROWS];
static int nshown;
static char prof_target[48];

static int p_rows(struct model *m, int parent) { return parent < 0 ? nshown : 0; }
static int p_columns(struct model *m) { return 4; }
static const char *p_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct prof_bucket *b = &shown[row];
    switch (col) {
    case 0: snprintf(buf, size, "%.1f", hist.total ? 100.0 * b->count / hist.total : 0.0); return buf;
    case 1: snprintf(buf, size, "%u", b->count); return buf;
    case 2: return b->key;
    default: return b->kernel == b->count ? "kernel" : b->kernel ? "mixed" : "user";
    }
}
static const char *p_header(struct model *m, int col)
{
    static const char *const names[] = { "%", "Samples", "Symbol", "Mode" };
    return names[col];
}
static struct model prof_model = { p_rows, m_child, p_columns, p_cell, p_header, NULL, NULL, NULL };

static const char *name_of_pid(int pid)
{
    for (int i = 0; i < nrows; i++)
        if (rows[i].pid == pid)
            return rows[i].name;
    return NULL;
}

static struct prof_symtab *symtab_for(const char *name, int pid)
{
    if (!name)
        return NULL;
    for (int i = 0; i < nsymtabs; i++)
        if (strcmp(symtabs[i].name, name) == 0)
            return symtabs[i].syms;
    if (nsymtabs == MAX_SYMTABS)
        return NULL;
    char path[64];
    snprintf(path, sizeof path, "/bin/%s", name);
    strncpy(symtabs[nsymtabs].name, name, sizeof symtabs[nsymtabs].name - 1);
    symtabs[nsymtabs].syms = prof_symtab_load_elf(path);
    prof_symtab_add_maps(symtabs[nsymtabs].syms, pid);
    return symtabs[nsymtabs++].syms;
}

static void prof_consume(int fd, int revents, void *arg)
{
    struct prof_sample buf[128];
    ssize_t n;
    int with_kernel = kernel_box->value;
    while ((n = prof_read(prof_fd, buf, 128)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            struct prof_sample *s = &buf[i];
            int kernel = !(s->flags & PROF_FLAG_USER);
            if (kernel)
                kernel_samples++;
            else
                user_samples++;
            if (kernel && !with_kernel)
                continue;
            char name[96];
            int locked;
            prof_attribute(kernel ? NULL : symtab_for(name_of_pid((int)s->pid), (int)s->pid), ksyms, s, &locked, name, sizeof name);
            if (locked)
                strncat(name, " (locked)", sizeof name - strlen(name) - 1);
            prof_hist_add(&hist, name, kernel);
        }
    }
}

static void prof_show(void)
{
    prof_hist_sort(&hist);
    nshown = hist.count < PROF_ROWS ? (int)hist.count : PROF_ROWS;
    memcpy(shown, hist.buckets, (size_t)nshown * sizeof shown[0]);
    view_refresh(prof_table);
    char text[160];
    struct prof_stats st = { 0 };
    if (prof_fd >= 0)
        prof_get_stats(prof_fd, &st);
    snprintf(text, sizeof text, "%s%s: %u user, %u kernel samples, %lu dropped", prof_target,
             st.enabled ? " (running)" : "", user_samples, kernel_samples, (unsigned long)st.dropped);
    widget_set_text(prof_status, text);
}

static int on_prof_start(struct widget *w, void *args, void *arg)
{
    if (prof_fd < 0) {
        prof_fd = prof_open();
        if (prof_fd < 0) {
            widget_set_text(prof_status, "cannot open /dev/profile");
            return 1;
        }
        prof_watch = app_watch_fd(app, prof_fd, POLLIN, prof_consume, NULL);
    }
    if (!ksyms)
        ksyms = prof_symtab_load_kernel();
    prof_hist_clear(&hist);
    kernel_samples = user_samples = 0;
    prof_pid = table->value >= 0 && table->value < nrows ? rows[table->value].pid : 0;
    if (prof_pid)
        snprintf(prof_target, sizeof prof_target, "pid %d (%s)", prof_pid, name_of_pid(prof_pid));
    else
        snprintf(prof_target, sizeof prof_target, "all processes");
    prof_start(prof_fd, prof_pid);
    printf("sysmon: profiling %s\n", prof_target);
    fflush(stdout);
    prof_show();
    return 1;
}

static int on_prof_stop(struct widget *w, void *args, void *arg)
{
    if (prof_fd >= 0) {
        prof_stop(prof_fd);
        prof_consume(prof_fd, POLLIN, NULL);
    }
    prof_show();
    return 1;
}

/* ---- refresh ---- */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char *text = malloc(8192);
    size_t n = text ? fread(text, 1, 8191, f) : 0;
    fclose(f);
    if (text)
        text[n] = '\0';
    return text;
}

static void refresh(void)
{
    int selected = table->value;
    long now = uptime_ms();
    memcpy(prev_rows, rows, sizeof rows);
    nprev = nrows;
    char *text = read_file("/dev/proc");
    nrows = 0;
    if (text) {
        char *line = strtok(text, "\n");
        while (line && nrows < MAX_PROCS) {
            struct proc_row r;
            char *end;
            r.pid = (int)strtol(line, &end, 10);
            r.ppid = (int)strtol(end, &end, 10);
            r.pgid = (int)strtol(end, &end, 10);
            while (*end == ' ') end++;
            int k = 0;
            while (*end && *end != ' ' && k < 11) r.state[k++] = *end++;
            r.state[k] = '\0';
            r.ticks = strtol(end, &end, 10);
            r.rss = strtol(end, &end, 10);
            while (*end == ' ') end++;
            k = 0;
            while (*end && *end != ' ' && k < 31) r.name[k++] = *end++;
            r.name[k] = '\0';
            r.cpu_pct = 0;
            long elapsed = now - prev_ms;
            for (int i = 0; i < nprev && elapsed > 0; i++)
                if (prev_rows[i].pid == r.pid) {
                    long delta = r.ticks - prev_rows[i].ticks;   /* ticks are milliseconds */
                    r.cpu_pct = (int)(delta * 1000 / elapsed);
                    break;
                }
            if (end != line && r.state[0] && r.name[0] && line[0] != 'P')
                rows[nrows++] = r;
            line = strtok(NULL, "\n");
        }
        free(text);
    }
    prev_ms = now;
    view_refresh(table);
    table->value = selected < nrows ? selected : -1;
    char *mem = read_file("/dev/meminfo");
    if (mem) {
        for (char *p = mem; *p; p++)
            if (*p == '\n')
                *p = ' ';
        widget_set_text(mem_label, mem);
        free(mem);
    }
    char s[64];
    snprintf(s, sizeof s, "%d processes, %d cpus, up %ld s", nrows, nproc(), uptime_ms() / 1000);
    widget_set_text(status, s);
    if (prof_fd >= 0)
        prof_show();
}

static void tick(void *arg) { refresh(); }

static int on_kill(struct widget *w, void *args, void *arg)
{
    int sig = (int)(long)arg;
    if (table->value >= 0 && table->value < nrows) {
        printf("sysmon: signal %d to pid %d\n", sig, rows[table->value].pid);
        fflush(stdout);
        kill(rows[table->value].pid, sig);
        refresh();
    }
    return 1;
}

int main(void)
{
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 560, 400, "sysmon");
    if (!win)
        return 1;
    struct widget *tabs = tabs_new(win);
    struct widget *page = tabs_add(tabs, "Processes");
    struct widget *bar = toolbar_new(page);
    widget_connect(button_new(bar, "Refresh"), "clicked", on_kill, (void *)0);
    widget_connect(button_new(bar, "Terminate"), "clicked", on_kill, (void *)SIGTERM);
    widget_connect(button_new(bar, "Kill"), "clicked", on_kill, (void *)SIGKILL);
    table = table_new(page);
    view_set_model(table, &model);
    table_set_column_width(table, 0, 50);
    table_set_column_width(table, 1, 50);
    table_set_column_width(table, 2, 50);
    table_set_column_width(table, 3, 70);
    table_set_column_width(table, 4, 60);
    table_set_column_width(table, 5, 70);
    table_set_column_width(table, 6, 160);
    mem_label = label_new(page, "");

    struct widget *ppage = tabs_add(tabs, "Profile");
    struct widget *pbar = toolbar_new(ppage);
    start_btn = button_new(pbar, "Start");
    widget_connect(start_btn, "clicked", on_prof_start, NULL);
    stop_btn = button_new(pbar, "Stop");
    widget_connect(stop_btn, "clicked", on_prof_stop, NULL);
    kernel_box = checkbox_new(pbar, "Kernel samples");
    kernel_box->value = 1;
    prof_status = label_new(ppage, "Select a process, then Start; without a selection every process is sampled.");
    prof_table = table_new(ppage);
    view_set_model(prof_table, &prof_model);
    table_set_column_width(prof_table, 0, 60);
    table_set_column_width(prof_table, 1, 80);
    table_set_column_width(prof_table, 2, 300);
    table_set_column_width(prof_table, 3, 70);

    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    prof_hist_init(&hist);
    refresh();
    app_timer_add(app, 1000, 1, tick, NULL);
    widget_focus(table);
    app_run(app);
    if (prof_fd >= 0) {
        prof_stop(prof_fd);
        close(prof_fd);
    }
    app_destroy(app);
    return 0;
}
