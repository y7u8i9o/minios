/* wireview: the protocol traffic between X12 and its clients, received
 * through the tracer interface of protocol/debug.xml. The window lists
 * every request and event with its client, object, message and
 * arguments, filters them by client and text, shows the selected message
 * with the history of its object, and draws the message rate of every
 * client over the last 30 seconds. "wireview -t" prints the trace on
 * standard output instead. The traffic of wireview itself is never
 * traced. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <gui/app.h>
#include <gui/client.h>
#include <gui/model.h>
#include <gui/theme.h>
#include "debug-client.h"

/* The last MSG_MAX messages are kept in a ring. A message is addressed
 * by its absolute number, the count of messages received before it. */
#define MSG_MAX 20000
#define CLIENTS_MAX 64
#define BUCKET_MS 250               /* width of one activity bucket */
#define BUCKETS 120                 /* 30 seconds of activity */
#define HISTORY_MAX 40              /* messages of the selected object shown in the detail pane */

struct msg {
    uint32_t seq, time_ms, client, object, size;
    uint8_t event;
    char iface[24], name[28];
    char *args;
};

struct wclient {
    uint32_t number, pid;
    int connected;
    char name[32];                  /* process name from /dev/proc, or empty */
    uint32_t requests, events;
    uint16_t req_bucket[BUCKETS], evt_bucket[BUCKETS];   /* ring indexed by the bucket clock */
};

static struct app *app;
static struct wire_proxy *tracer;
static struct msg ring[MSG_MAX];
static uint32_t total;              /* messages received since the last clear */
static uint32_t first_ms;           /* server time of the first message */
static uint32_t dropped;
static struct wclient clients[CLIENTS_MAX];
static int nclients;
static unsigned bucket_now;         /* the bucket that receives new messages */

/* The shown rows: absolute message numbers in order. */
static uint32_t *shown;
static int nshown, shown_cap;

static struct widget *table, *detail, *activity, *status, *client_combo, *filter_field, *hide_box, *follow_box, *record_box;
static int combo_clients[CLIENTS_MAX];   /* client number of each combo entry after "All clients" */
static int ncombo;
static int dirty, selected = -1;

/* Text mode. */
static int text_mode, text_limit, text_count, text_done;
static int text_client = -1;

static struct msg *msg_at(uint32_t n) { return &ring[n % MSG_MAX]; }
static uint32_t oldest(void) { return total > MSG_MAX ? total - MSG_MAX : 0; }

static struct wclient *client_find(uint32_t number, int create)
{
    for (int i = 0; i < nclients; i++)
        if (clients[i].number == number)
            return &clients[i];
    if (!create || nclients == CLIENTS_MAX)
        return NULL;
    struct wclient *c = &clients[nclients++];
    memset(c, 0, sizeof *c);
    c->number = number;
    return c;
}

/* The name of a process from the NAME column of /dev/proc, whose rows are
 * "PID PPID PGID STATE TIME RSS NAME". */
static void process_name(uint32_t pid, char *out, size_t size)
{
    out[0] = '\0';
    if (!pid)
        return;
    int fd = open("/dev/proc", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    static char buf[16384];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        while (*line == ' ')
            line++;
        if ((uint32_t)atoi(line) == pid) {
            char *name = strrchr(line, ' ');
            strlcpy(out, name ? name + 1 : line, size);
            return;
        }
        line = next;
    }
}

static void client_label(const struct wclient *c, char *buf, size_t size)
{
    if (c->name[0])
        snprintf(buf, size, "%u %s", c->number, c->name);
    else
        snprintf(buf, size, "client %u", c->number);
}

/* ---- filtering ---- */

/* Messages sent for every frame of an animated or redrawing client,
 * which "Hide frame traffic" removes. */
static int frame_traffic(const struct msg *m)
{
    static const char *const names[][2] = {
        { "callback", NULL }, { "surface", "frame" }, { "surface", "attach" }, { "surface", "damage" },
        { "surface", "commit" }, { "buffer", "release" }, { "display", "delete_id" },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(m->iface, names[i][0]) == 0 && (!names[i][1] || strcmp(m->name, names[i][1]) == 0))
            return 1;
    return 0;
}

static int client_filter(void)
{
    int v = client_combo ? client_combo->value : 0;
    return v > 0 && v <= ncombo ? combo_clients[v - 1] : -1;
}

static int matches(const struct msg *m)
{
    int only = client_filter();
    if (only >= 0 && m->client != (uint32_t)only)
        return 0;
    if (hide_box && hide_box->value && frame_traffic(m))
        return 0;
    const char *needle = filter_field ? widget_text(filter_field) : "";
    if (!needle[0])
        return 1;
    char line[400];
    snprintf(line, sizeof line, "%s@%u.%s(%s)", m->iface, m->object, m->name, m->args ? m->args : "");
    return strstr(line, needle) != NULL;
}

static void shown_add(uint32_t n)
{
    if (nshown == shown_cap) {
        shown_cap = shown_cap ? shown_cap * 2 : 1024;
        shown = realloc(shown, (size_t)shown_cap * sizeof *shown);
    }
    shown[nshown++] = n;
}

/* Rows whose message the ring has overwritten leave the front. */
static void shown_trim(void)
{
    int k = 0;
    while (k < nshown && shown[k] < oldest())
        k++;
    if (k) {
        memmove(shown, shown + k, (size_t)(nshown - k) * sizeof *shown);
        nshown -= k;
        selected = selected >= k ? selected - k : -1;
    }
}

static void rebuild(void)
{
    nshown = 0;
    selected = -1;
    for (uint32_t n = oldest(); n < total; n++)
        if (matches(msg_at(n)))
            shown_add(n);
    dirty = 1;
}

/* ---- the table model ---- */

static int m_rows(struct model *m, int parent) { return parent < 0 ? nshown : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 7; }
static const char *m_cell(struct model *mo, int row, int col, char *buf, size_t size)
{
    if (row < 0 || row >= nshown)
        return "";
    const struct msg *m = msg_at(shown[row]);
    switch (col) {
    case 0: {
        uint32_t t = m->time_ms - first_ms;
        snprintf(buf, size, "%u.%03u", t / 1000, t % 1000);
        return buf;
    }
    case 1: {
        struct wclient *c = client_find(m->client, 0);
        if (c)
            client_label(c, buf, size);
        else
            snprintf(buf, size, "%u", m->client);
        return buf;
    }
    case 2: return m->event ? "event" : "request";
    case 3: snprintf(buf, size, "%s@%u", m->iface, m->object); return buf;
    case 4: return m->name;
    case 5: return m->args ? m->args : "";
    default: snprintf(buf, size, "%u", m->size); return buf;
    }
}
static const char *m_header(struct model *m, int col)
{
    static const char *const names[] = { "Time", "Client", "Kind", "Object", "Message", "Arguments", "Bytes" };
    return names[col];
}
static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL, NULL };

/* ---- the detail pane ---- */

static void append(char **text, size_t *len, size_t *cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void append(char **text, size_t *len, size_t *cap, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = sizeof line - 1;
    if (*len + (size_t)n + 1 > *cap) {
        *cap = (*len + (size_t)n + 1) * 2;
        *text = realloc(*text, *cap);
    }
    memcpy(*text + *len, line, (size_t)n + 1);
    *len += (size_t)n;
}

static void show_detail(void)
{
    if (!detail)
        return;
    if (selected < 0 || selected >= nshown) {
        editor_set_text(detail, "Select a message to see its arguments and the history of its object.");
        return;
    }
    uint32_t n = shown[selected];
    const struct msg *m = msg_at(n);
    char *text = NULL, who[48] = "";
    size_t len = 0, cap = 0;
    struct wclient *c = client_find(m->client, 0);
    if (c)
        client_label(c, who, sizeof who);
    uint32_t t = m->time_ms - first_ms;
    append(&text, &len, &cap, "At %u.%03u s from %s", t / 1000, t % 1000, who[0] ? who : "an unknown client");
    if (c && c->pid)
        append(&text, &len, &cap, ", process %u", c->pid);
    append(&text, &len, &cap, "\nSequence number %u in X12's trace\n", m->seq);
    append(&text, &len, &cap, "%s %s@%u.%s, %u bytes\n", m->event ? "Event" : "Request", m->iface, m->object, m->name,
           m->size);
    append(&text, &len, &cap, "Arguments: %s\n\n", m->args && m->args[0] ? m->args : "none");
    /* The history of the object: the messages of this client on the same
     * object id, newest last, since the last message that created it. */
    uint32_t hits[HISTORY_MAX];
    int nhits = 0;
    for (uint32_t k = n + 1; k-- > oldest() && nhits < HISTORY_MAX;) {
        const struct msg *o = msg_at(k);
        if (o->client == m->client && o->object == m->object && strcmp(o->iface, m->iface) == 0)
            hits[nhits++] = k;
    }
    append(&text, &len, &cap, "History of %s@%u up to this message (%d shown):\n", m->iface, m->object, nhits);
    for (int i = nhits - 1; i >= 0; i--) {
        const struct msg *o = msg_at(hits[i]);
        uint32_t ot = o->time_ms - first_ms;
        append(&text, &len, &cap, "%6u.%03u  %-7s %s(%s)\n", ot / 1000, ot % 1000, o->event ? "event" : "request", o->name,
               o->args ? o->args : "");
    }
    editor_set_text(detail, text ? text : "");
    free(text);
}

/* ---- activity ---- */

static void bucket_advance(void *arg)
{
    bucket_now++;
    for (int i = 0; i < nclients; i++) {
        clients[i].req_bucket[bucket_now % BUCKETS] = 0;
        clients[i].evt_bucket[bucket_now % BUCKETS] = 0;
    }
    if (activity)
        widget_invalidate(activity);
}

/* One row per client: its name and totals above a strip of bars, one per
 * quarter second, requests in the accent colour stacked on events. */
static int on_activity_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *th = widget_theme(w);
    painter_fill(p, 0, 0, w->w, w->h, th->color[TC_FIELD]);
    int th_h = painter_text_height(p);
    int row_h = th_h + 30, y = 4;
    int bar_w = (w->w - 16) / BUCKETS;
    if (bar_w < 1)
        bar_w = 1;
    if (!nclients)
        painter_text(p, 8, y, "No client traffic yet.", th->color[TC_TEXT_DISABLED]);
    for (int i = 0; i < nclients && y + row_h <= w->h; i++, y += row_h) {
        const struct wclient *c = &clients[i];
        char label[48], line[96];
        client_label(c, label, sizeof label);
        snprintf(line, sizeof line, "%s%s  %u requests, %u events", label, c->connected ? "" : " (gone)", c->requests,
                 c->events);
        painter_text(p, 8, y, line, c->connected ? th->color[TC_TEXT] : th->color[TC_TEXT_DISABLED]);
        int base = y + th_h + 26, peak = 1;
        for (int b = 0; b < BUCKETS; b++) {
            int v = c->req_bucket[b] + c->evt_bucket[b];
            if (v > peak)
                peak = v;
        }
        painter_frame(p, 7, y + th_h + 1, BUCKETS * bar_w + 2, 26, th->color[TC_BORDER]);
        for (int b = 0; b < BUCKETS; b++) {
            unsigned slot = (bucket_now + 1 + (unsigned)b) % BUCKETS;   /* oldest on the left */
            int evh = c->evt_bucket[slot] * 24 / peak, rqh = c->req_bucket[slot] * 24 / peak;
            int x = 8 + b * bar_w;
            if (evh)
                painter_fill(p, x, base - evh, bar_w, evh, th->color[TC_BORDER]);
            if (rqh)
                painter_fill(p, x, base - evh - rqh, bar_w, rqh, th->color[TC_ACCENT]);
        }
    }
    return 1;
}

/* ---- tracer events ---- */

static void update_status(void)
{
    if (!status)
        return;
    char s[128];
    snprintf(s, sizeof s, "%u messages, %d shown, %u dropped, %d clients%s", total, nshown, dropped, nclients,
             record_box && !record_box->value ? ", paused" : "");
    widget_set_text(status, s);
}

static void refresh_clients(void)
{
    if (!client_combo)
        return;
    int keep = client_filter();
    combobox_clear(client_combo);
    combobox_add(client_combo, "All clients");
    ncombo = 0;
    int select = 0;
    for (int i = 0; i < nclients; i++) {
        char label[48];
        client_label(&clients[i], label, sizeof label);
        combobox_add(client_combo, label);
        combo_clients[ncombo++] = (int)clients[i].number;
        if ((int)clients[i].number == keep)
            select = ncombo;
    }
    combobox_select(client_combo, select);
}

static void on_client(void *user, struct wire_proxy *p, uint32_t number, uint32_t pid, uint32_t connected)
{
    struct wclient *c = client_find(number, 1);
    if (!c)
        return;
    c->connected = (int)connected;
    if (pid && pid != c->pid) {
        c->pid = pid;
        process_name(pid, c->name, sizeof c->name);
    }
    if (text_mode) {
        if (text_client < 0 || (uint32_t)text_client == number)
            printf("wireview: client %u%s%s %s%s\n", number, c->name[0] ? " " : "", c->name,
                   connected ? "connected" : "disconnected", pid ? "" : ", pid not yet known");
        fflush(stdout);
        return;
    }
    refresh_clients();
    dirty = 1;
}

static void on_message(void *user, struct wire_proxy *p, uint32_t seq, uint32_t time_ms, uint32_t client,
                       uint32_t direction, uint32_t object, const char *iface, const char *name, const char *args,
                       uint32_t size)
{
    if (!total)
        first_ms = time_ms;
    if (text_mode) {
        if (text_client >= 0 && client != (uint32_t)text_client)
            return;
        uint32_t t = time_ms - first_ms;
        printf("[%5u.%03u] %u %s %s@%u.%s(%s)\n", t / 1000, t % 1000, client, direction ? "<-" : "->", iface, object, name,
               args);
        fflush(stdout);
        total++;
        if (text_limit && ++text_count >= text_limit)
            text_done = 1;
        return;
    }
    struct msg *m = msg_at(total);
    free(m->args);
    m->seq = seq;
    m->time_ms = time_ms;
    m->client = client;
    m->object = object;
    m->size = size;
    m->event = (uint8_t)direction;
    strlcpy(m->iface, iface, sizeof m->iface);
    strlcpy(m->name, name, sizeof m->name);
    m->args = strdup(args);
    uint32_t n = total++;
    struct wclient *c = client_find(client, 1);
    if (c) {
        if (direction) {
            c->events++;
            c->evt_bucket[bucket_now % BUCKETS]++;
        } else {
            c->requests++;
            c->req_bucket[bucket_now % BUCKETS]++;
        }
    }
    shown_trim();
    if (matches(m))
        shown_add(n);
    dirty = 1;
}

static void on_dropped(void *user, struct wire_proxy *p, uint32_t count)
{
    dropped += count;
    if (text_mode) {
        printf("wireview: %u messages dropped\n", count);
        fflush(stdout);
    }
    dirty = 1;
}
static const struct tracer_listener tracer_events = { on_client, on_message, on_dropped };

/* The view is refreshed ten times a second at most, so that a burst of
 * traffic costs one repaint. */
static void refresh(void *arg)
{
    if (!dirty)
        return;
    dirty = 0;
    view_refresh(table);
    if (follow_box->value && nshown)
        view_select(table, nshown - 1);
    update_status();
    widget_invalidate(activity);
}

/* ---- controls ---- */

static int on_select(struct widget *w, void *args, void *arg)
{
    selected = ((struct sig_select *)args)->index;
    show_detail();
    return 0;
}
static int on_filter(struct widget *w, void *args, void *arg) { rebuild(); refresh(NULL); return 0; }
static int on_record(struct widget *w, void *args, void *arg)
{
    if (w->value)
        tracer_start(tracer);
    else
        tracer_stop(tracer);
    gui_flush();
    update_status();
    return 0;
}
static int on_clear(struct widget *w, void *args, void *arg)
{
    for (uint32_t n = oldest(); n < total; n++) {
        free(msg_at(n)->args);
        msg_at(n)->args = NULL;
    }
    total = 0;
    dropped = 0;
    nshown = 0;
    selected = -1;
    for (int i = 0; i < nclients; i++) {
        clients[i].requests = clients[i].events = 0;
        memset(clients[i].req_bucket, 0, sizeof clients[i].req_bucket);
        memset(clients[i].evt_bucket, 0, sizeof clients[i].evt_bucket);
    }
    show_detail();
    dirty = 1;
    refresh(NULL);
    return 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: wireview [-t] [-n COUNT] [-c CLIENT]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "tn:c:")) != -1) {
        switch (opt) {
        case 't': text_mode = 1; break;
        case 'n': text_limit = atoi(optarg); break;
        case 'c': text_client = atoi(optarg); break;
        default: usage();
        }
    }
    app = app_create();
    if (!app)
        return 1;
    tracer = gui_bind_global("tracer", &tracer_interface, 1);
    if (!tracer) {
        fprintf(stderr, "wireview: X12 has no tracer interface\n");
        return 1;
    }
    tracer_add_listener(tracer, &tracer_events, NULL);
    if (text_mode) {
        printf("wireview: tracing\n");
        fflush(stdout);
        tracer_start(tracer);
        gui_flush();
        /* app_run returns at once without a window, so the text mode
         * waits for events itself until the count is reached or X12
         * goes away. */
        struct wmsg ev;
        while (!text_done && gui_next_event(&ev, 1000) >= 0)
            ;
        tracer_destroy(tracer);
        gui_flush();
        app_destroy(app);
        return 0;
    }

    struct widget *win = app_window(app, 900, 560, "Protocol viewer");
    if (!win)
        return 1;
    struct widget *bar = toolbar_new(win);
    record_box = checkbox_new(bar, "Record");
    record_box->value = 1;
    widget_connect(record_box, "toggled", on_record, NULL);
    widget_connect(button_new(bar, "Clear"), "clicked", on_clear, NULL);
    client_combo = combobox_new(bar);
    widget_set_hint(client_combo, 150, 0);
    widget_connect(client_combo, "changed", on_filter, NULL);
    label_new(bar, "Filter:");
    filter_field = textfield_new(bar, "");
    widget_set_hint(filter_field, 160, 0);
    widget_connect(filter_field, "changed", on_filter, NULL);
    hide_box = checkbox_new(bar, "Hide frame traffic");
    widget_connect(hide_box, "toggled", on_filter, NULL);
    follow_box = checkbox_new(bar, "Follow");
    follow_box->value = 1;

    struct widget *split = splitpane_new(win, 1);
    widget_set_stretch(split, 1, 1);
    table = table_new(split);
    view_set_model(table, &model);
    static const int widths[] = { 70, 90, 60, 120, 110, 330, 50 };
    for (int i = 0; i < 7; i++)
        table_set_column_width(table, i, widths[i]);
    widget_connect(table, "selected", on_select, NULL);
    struct widget *lower = splitpane_new(split, 0);
    detail = editor_new(lower);
    editor_set_readonly(detail, 1);
    activity = canvas_new(lower);
    widget_connect(activity, "paint", on_activity_paint, NULL);
    splitpane_set_position(split, 300);
    splitpane_set_position(lower, 440);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);

    refresh_clients();
    show_detail();
    tracer_start(tracer);
    gui_flush();
    update_status();
    app_timer_add(app, 100, 1, refresh, NULL);
    app_timer_add(app, BUCKET_MS, 1, bucket_advance, NULL);
    app_run(app);
    printf("wireview: %u messages from %d clients, %u dropped\n", total, nclients, dropped);
    fflush(stdout);
    app_destroy(app);
    return 0;
}
