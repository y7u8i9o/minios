/* sysmon: processes and memory, refreshed every second. The process
 * table comes from /dev/proc, memory from /dev/meminfo; a selected
 * process can be sent SIGTERM or SIGKILL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <gui/app.h>
#include <gui/model.h>

#define MAX_PROCS 64

struct proc_row { int pid, ppid, pgid; char state[12], name[32]; };

static struct app *app;
static struct widget *table, *mem_label, *status;
static struct proc_row rows[MAX_PROCS];
static int nrows;

static int m_rows(struct model *m, int parent) { return parent < 0 ? nrows : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 5; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct proc_row *r = &rows[row];
    switch (col) {
    case 0: snprintf(buf, size, "%d", r->pid); return buf;
    case 1: snprintf(buf, size, "%d", r->ppid); return buf;
    case 2: snprintf(buf, size, "%d", r->pgid); return buf;
    case 3: return r->state;
    default: return r->name;
    }
}
static const char *m_header(struct model *m, int col)
{
    static const char *const names[] = { "PID", "PPID", "PGID", "State", "Name" };
    return names[col];
}
static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL };

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
            while (*end == ' ') end++;
            k = 0;
            while (*end && *end != ' ' && k < 31) r.name[k++] = *end++;
            r.name[k] = '\0';
            if (end != line && r.state[0] && r.name[0] && line[0] != 'P')
                rows[nrows++] = r;
            line = strtok(NULL, "\n");
        }
        free(text);
    }
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
    struct widget *win = app_window(app, 520, 360, "sysmon");
    if (!win)
        return 1;
    struct widget *bar = toolbar_new(win);
    widget_connect(button_new(bar, "Refresh"), "clicked", on_kill, (void *)0);
    widget_connect(button_new(bar, "Terminate"), "clicked", on_kill, (void *)SIGTERM);
    widget_connect(button_new(bar, "Kill"), "clicked", on_kill, (void *)SIGKILL);
    table = table_new(win);
    view_set_model(table, &model);
    table_set_column_width(table, 0, 60);
    table_set_column_width(table, 1, 60);
    table_set_column_width(table, 2, 60);
    table_set_column_width(table, 3, 90);
    table_set_column_width(table, 4, 200);
    mem_label = label_new(win, "");
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    refresh();
    app_timer_add(app, 1000, 1, tick, NULL);
    widget_focus(table);
    app_run(app);
    app_destroy(app);
    return 0;
}
