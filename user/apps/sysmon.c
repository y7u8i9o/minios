/* sysmon: processes and memory, refreshed every second. The process table
 * comes from /dev/proc (with the CPU share computed from the TIME column
 * between refreshes and the resident size from RSS), memory from
 * /dev/meminfo; a selected process can be sent SIGTERM or SIGKILL, or
 * handed to profiler(1), which grew out of this window and now owns
 * everything about sampling. */
#include <minios/local.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/wait.h>
#include <gui/app.h>
#include <gui/model.h>

#define MAX_PROCS 64

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

/* The profiler is a separate program; sysmon hands it the selection.
 * The intermediate child is reaped at once so no zombie is left behind. */
static int on_profile(struct widget *w, void *args, void *arg)
{
    int selected = table->value;
    char pid[16] = "";
    if (selected >= 0 && selected < nrows)
        snprintf(pid, sizeof pid, "%d", rows[selected].pid);
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
    struct widget *page = box_new(win, 1);
    struct widget *bar = toolbar_new(page);
    widget_connect(button_new(bar, "Refresh"), "clicked", on_kill, (void *)0);
    widget_connect(button_new(bar, "Terminate"), "clicked", on_kill, (void *)SIGTERM);
    widget_connect(button_new(bar, "Kill"), "clicked", on_kill, (void *)SIGKILL);
    widget_connect(button_new(bar, "Profile"), "clicked", on_profile, NULL);
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

    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    refresh();
    app_timer_add(app, 1000, 1, tick, NULL);
    widget_focus(table);
    app_run(app);
    app_destroy(app);
    return 0;
}
