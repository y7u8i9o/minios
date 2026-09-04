/* Sound, date and time, file types, launcher and system pages. */
#include "settings.h"
#include <gui/client.h>
#include <gui/mime.h>
#include <audio/audio.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/time.h>
#include <sys/utsname.h>

/* ---- sound ---- */

static struct audio_connection *audio;
static struct audio_mixer *mixer;
static struct widget *master_slider, *master_label, *stream_table, *stream_slider, *stream_label, *sound_status;
static uint32_t shown_generation;
static int sound_building;

static int s_rows(struct model *m, int parent) { return parent < 0 && mixer ? audio_mixer_count(mixer) : 0; }
static int s_child(struct model *m, int parent, int index) { return index; }
static int s_columns(struct model *m) { return 4; }
static const char *s_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    const struct audio_mixer_stream *s = audio_mixer_stream(mixer, row);
    if (!s)
        return "";
    switch (col) {
    case 0: return s->name;
    case 1: return s->direction ? "capture" : "playback";
    case 2: snprintf(buf, size, "%u %%", s->volume); return buf;
    default: return s->state == AUDIO_PLAYBACK_RUNNING ? "running" : s->state == AUDIO_PLAYBACK_ERROR ? "error" : "paused";
    }
}
static const char *s_header(struct model *m, int col)
{
    static const char *const names[] = { "Stream", "Direction", "Volume", "State" };
    return names[col];
}
static struct model stream_model = { s_rows, s_child, s_columns, s_cell, s_header, NULL, NULL };

static void sound_refresh(void)
{
    if (!mixer)
        return;
    sound_building = 1;
    char text[64];
    snprintf(text, sizeof text, "Output volume: %u %%", audio_mixer_master(mixer));
    widget_set_text(master_label, text);
    widget_set_value(master_slider, (int)audio_mixer_master(mixer));
    view_refresh(stream_table);
    int sel = stream_table->value;
    const struct audio_mixer_stream *s = sel >= 0 ? audio_mixer_stream(mixer, sel) : NULL;
    if (s) {
        snprintf(text, sizeof text, "%s: %u %%", s->name, s->volume);
        widget_set_text(stream_label, text);
        widget_set_value(stream_slider, (int)s->volume);
        widget_set_enabled(stream_slider, 1);
    } else {
        widget_set_text(stream_label, "Stream volume");
        widget_set_enabled(stream_slider, 0);
    }
    snprintf(text, sizeof text, "%d stream%s", audio_mixer_count(mixer), audio_mixer_count(mixer) == 1 ? "" : "s");
    widget_set_text(sound_status, text);
    sound_building = 0;
}

static void on_audio(int fd, int revents, void *arg)
{
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) || audio_connection_dispatch(audio, 0) < 0) {
        widget_set_text(sound_status, "No audio server");
        mixer = NULL;
        return;
    }
    if (audio_mixer_generation(mixer) != shown_generation) {
        shown_generation = audio_mixer_generation(mixer);
        sound_refresh();
    }
}
static int on_master(struct widget *w, void *args, void *arg)
{
    if (sound_building || !mixer) return 1;
    audio_mixer_set_master(mixer, (unsigned)w->value);
    char text[64];
    snprintf(text, sizeof text, "Output volume: %d %%", w->value);
    widget_set_text(master_label, text);
    return 1;
}
static int on_stream_volume(struct widget *w, void *args, void *arg)
{
    if (sound_building || !mixer) return 1;
    const struct audio_mixer_stream *s = stream_table->value >= 0 ? audio_mixer_stream(mixer, stream_table->value) : NULL;
    if (s)
        audio_mixer_set_volume(mixer, s->id, (unsigned)w->value);
    return 1;
}
static int on_stream_selected(struct widget *w, void *args, void *arg) { sound_refresh(); return 1; }

void build_sound(struct widget *page)
{
    master_label = label_new(page, "Output volume");
    master_slider = slider_new(page, 0, 100, 100);
    widget_set_stretch(master_slider, 1, 0);
    widget_connect(master_slider, "changed", on_master, NULL);
    label_new(page, "Streams");
    stream_table = table_new(page);
    view_set_model(stream_table, &stream_model);
    table_set_column_width(stream_table, 0, 200);
    table_set_column_width(stream_table, 1, 80);
    table_set_column_width(stream_table, 2, 70);
    table_set_column_width(stream_table, 3, 80);
    widget_set_stretch(stream_table, 1, 1);
    widget_connect(stream_table, "selected", on_stream_selected, NULL);
    stream_label = label_new(page, "Stream volume");
    stream_slider = slider_new(page, 0, 100, 100);
    widget_set_stretch(stream_slider, 1, 0);
    widget_set_enabled(stream_slider, 0);
    widget_connect(stream_slider, "changed", on_stream_volume, NULL);
    sound_status = label_new(page, "");
    audio = audio_connect();
    mixer = audio ? audio_mixer_create(audio) : NULL;
    if (!mixer) {
        widget_set_text(sound_status, "No audio server");
        widget_set_enabled(master_slider, 0);
        return;
    }
    app_watch_fd(app, audio_connection_fd(audio), POLLIN, on_audio, NULL);
    audio_mixer_sync(mixer);
    shown_generation = audio_mixer_generation(mixer);
    sound_refresh();
}

/* ---- date and time ---- */

static struct widget *now_label, *fields[6];
static const struct { const char *name; int min, max; } field_defs[6] = {
    { "Year", 2000, 2099 }, { "Month", 1, 12 }, { "Day", 1, 31 }, { "Hour", 0, 23 }, { "Minute", 0, 59 }, { "Second", 0, 59 },
};

static void time_tick(void *arg)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    char text[80];
    strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S UTC", &tm);
    widget_set_text(now_label, text);
}
static int on_load_now(struct widget *w, void *args, void *arg)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    int v[6] = { tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec };
    for (int i = 0; i < 6; i++)
        widget_set_value(fields[i], v[i]);
    return 1;
}
static int on_set_time(struct widget *w, void *args, void *arg)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = fields[0]->value - 1900;
    tm.tm_mon = fields[1]->value - 1;
    tm.tm_mday = fields[2]->value;
    tm.tm_hour = fields[3]->value;
    tm.tm_min = fields[4]->value;
    tm.tm_sec = fields[5]->value;
    struct timeval tv = { timegm(&tm), 0 };
    if (settimeofday(&tv, NULL) < 0) {
        static const char *const buttons[] = { "OK" };
        app_dialog(app, "Date and time", "The clock could not be set.", buttons, 1);
        return 1;
    }
    printf("settings: clock set to %ld\n", (long)tv.tv_sec);
    fflush(stdout);
    time_tick(NULL);
    return 1;
}

void build_datetime(struct widget *page)
{
    now_label = label_new(page, "");
    widget_set_hint(now_label, 0, 28);
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    for (int i = 0; i < 6; i++) {
        row_label(grid, i, field_defs[i].name);
        fields[i] = spinner_new(grid, field_defs[i].min, field_defs[i].max, field_defs[i].min);
        widget_set_grid(fields[i], i, 1, 1, 1);
    }
    struct widget *row = box_new(page, 0);
    widget_connect(button_new(row, "Load current time"), "clicked", on_load_now, NULL);
    widget_connect(button_new(row, "Set clock"), "clicked", on_set_time, NULL);
    on_load_now(NULL, NULL, NULL);
    time_tick(NULL);
    app_timer_add(app, 1000, 1, time_tick, NULL);
}

/* ---- file types ---- */

static struct widget *apps_table, *prog_field, *type_label;

static int m_rows(struct model *m, int parent) { return parent < 0 ? mime_handler_count() : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 2; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    return col == 0 ? mime_handler_type(row) : mime_handler_program(row);
}
static const char *m_header(struct model *m, int col) { return col == 0 ? "Type" : "Program"; }
static struct model apps_model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL };

static int on_type_selected(struct widget *w, void *args, void *arg)
{
    int i = ((struct sig_select *)args)->index;
    if (i >= 0 && i < mime_handler_count()) {
        widget_set_text(type_label, mime_handler_type(i));
        widget_set_text(prog_field, mime_handler_program(i));
    }
    return 1;
}
static int on_set_program(struct widget *w, void *args, void *arg)
{
    int i = apps_table->value;
    if (i < 0 || i >= mime_handler_count() || !widget_text(prog_field)[0])
        return 1;
    mime_set_handler(mime_handler_type(i), widget_text(prog_field));
    if (mime_save(NULL) < 0)
        printf("settings: cannot save the file type table\n");
    else
        printf("settings: handler %s = %s\n", mime_handler_type(i), widget_text(prog_field));
    fflush(stdout);
    view_refresh(apps_table);
    return 1;
}
static int on_add_type(struct widget *w, void *args, void *arg)
{
    char type[64] = "";
    if (!app_prompt(app, "New file type", "Type (for example text/x-log):", type, sizeof type) || !type[0])
        return 1;
    mime_set_handler(type, "/bin/gedit");
    mime_save(NULL);
    view_refresh(apps_table);
    return 1;
}

void build_filetypes(struct widget *page)
{
    apps_table = table_new(page);
    view_set_model(apps_table, &apps_model);
    table_set_column_width(apps_table, 0, 220);
    table_set_column_width(apps_table, 1, 220);
    widget_set_stretch(apps_table, 1, 1);
    widget_connect(apps_table, "selected", on_type_selected, NULL);
    struct widget *row = box_new(page, 0);
    widget_set_stretch(row, 1, 0);
    type_label = label_new(row, "(select a type)");
    widget_set_hint(type_label, 160, 0);
    prog_field = textfield_new(row, "");
    widget_set_stretch(prog_field, 1, 0);
    widget_connect(prog_field, "activate", on_set_program, NULL);
    widget_connect(button_new(row, "Set"), "clicked", on_set_program, NULL);
    widget_connect(button_new(row, "Add type..."), "clicked", on_add_type, NULL);
}

/* ---- launcher ---- */

struct entry { char title[48]; char program[128]; };
static struct entry entries[32];
static int nentries;
static struct widget *launch_table, *title_field, *program_field;

static void launcher_read(void)
{
    nentries = 0;
    FILE *f = fopen(LAUNCHER_PATH, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f) && nentries < 32) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = '\0';
        strlcpy(entries[nentries].title, line, sizeof entries[0].title);
        strlcpy(entries[nentries].program, eq + 1, sizeof entries[0].program);
        nentries++;
    }
    fclose(f);
}
static int launcher_write(void)
{
    FILE *f = fopen(LAUNCHER_PATH, "w");
    if (!f)
        return -1;
    fprintf(f, "# Launcher menu of the window server: title=program\n");
    for (int i = 0; i < nentries; i++)
        fprintf(f, "%s=%s\n", entries[i].title, entries[i].program);
    fclose(f);
    printf("settings: launcher saved with %d entries\n", nentries);
    fflush(stdout);
    return 0;
}
static int l_rows(struct model *m, int parent) { return parent < 0 ? nentries : 0; }
static int l_child(struct model *m, int parent, int index) { return index; }
static int l_columns(struct model *m) { return 2; }
static const char *l_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    return col == 0 ? entries[row].title : entries[row].program;
}
static const char *l_header(struct model *m, int col) { return col == 0 ? "Menu entry" : "Program"; }
static struct model launch_model = { l_rows, l_child, l_columns, l_cell, l_header, NULL, NULL };

static int on_entry_selected(struct widget *w, void *args, void *arg)
{
    int i = ((struct sig_select *)args)->index;
    if (i >= 0 && i < nentries) {
        widget_set_text(title_field, entries[i].title);
        widget_set_text(program_field, entries[i].program);
    }
    return 1;
}
static int on_entry_save(struct widget *w, void *args, void *arg)
{
    int i = launch_table->value;
    if (!widget_text(title_field)[0] || !widget_text(program_field)[0])
        return 1;
    if (i < 0 || i >= nentries) {
        if (nentries == 32)
            return 1;
        i = nentries++;
    }
    strlcpy(entries[i].title, widget_text(title_field), sizeof entries[i].title);
    strlcpy(entries[i].program, widget_text(program_field), sizeof entries[i].program);
    launcher_write();
    view_refresh(launch_table);
    return 1;
}
static int on_entry_add(struct widget *w, void *args, void *arg)
{
    launch_table->value = -1;
    widget_set_text(title_field, "");
    widget_set_text(program_field, "/bin/");
    widget_focus(title_field);
    return 1;
}
static int on_entry_remove(struct widget *w, void *args, void *arg)
{
    int i = launch_table->value;
    if (i < 0 || i >= nentries)
        return 1;
    memmove(&entries[i], &entries[i + 1], (size_t)(nentries - i - 1) * sizeof entries[0]);
    nentries--;
    launch_table->value = -1;
    launcher_write();
    view_refresh(launch_table);
    return 1;
}
static int on_entry_move(struct widget *w, void *args, void *arg)
{
    int i = launch_table->value, d = (int)(long)arg;
    if (i < 0 || i >= nentries || i + d < 0 || i + d >= nentries)
        return 1;
    struct entry t = entries[i];
    entries[i] = entries[i + d];
    entries[i + d] = t;
    launch_table->value = i + d;
    launcher_write();
    view_refresh(launch_table);
    return 1;
}

void build_launcher(struct widget *page)
{
    launcher_read();
    launch_table = table_new(page);
    view_set_model(launch_table, &launch_model);
    table_set_column_width(launch_table, 0, 180);
    table_set_column_width(launch_table, 1, 260);
    widget_set_stretch(launch_table, 1, 1);
    widget_connect(launch_table, "selected", on_entry_selected, NULL);
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    row_label(grid, 0, "Menu entry");
    title_field = textfield_new(grid, "");
    widget_set_grid(title_field, 0, 1, 1, 1);
    row_label(grid, 1, "Program");
    program_field = textfield_new(grid, "");
    widget_set_grid(program_field, 1, 1, 1, 1);
    widget_connect(program_field, "activate", on_entry_save, NULL);
    struct widget *row = box_new(page, 0);
    widget_connect(button_new(row, "Save entry"), "clicked", on_entry_save, NULL);
    widget_connect(button_new(row, "New"), "clicked", on_entry_add, NULL);
    widget_connect(button_new(row, "Remove"), "clicked", on_entry_remove, NULL);
    widget_connect(button_new(row, "Move up"), "clicked", on_entry_move, (void *)-1L);
    widget_connect(button_new(row, "Move down"), "clicked", on_entry_move, (void *)1L);
}

/* ---- system ---- */

static struct widget *uptime_label, *mem_label;

static long meminfo_kb(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? atol(p + strlen(key)) : 0;
}

static void system_tick(void *arg)
{
    long s = uptime_ms() / 1000;
    char text[160];
    snprintf(text, sizeof text, "Up %ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    widget_set_text(uptime_label, text);
    int fd = open("/dev/meminfo", O_RDONLY);
    if (fd < 0)
        return;
    char buf[512];
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    long total = meminfo_kb(buf, "MemTotal:"), free_kb = meminfo_kb(buf, "MemFree:");
    long stotal = meminfo_kb(buf, "SwapTotal:"), sfree = meminfo_kb(buf, "SwapFree:");
    snprintf(text, sizeof text, "Memory %ld of %ld MiB in use, swap %ld of %ld MiB in use", (total - free_kb) / 1024,
             total / 1024, (stotal - sfree) / 1024, stotal / 1024);
    widget_set_text(mem_label, text);
}
static int on_launch(struct widget *w, void *args, void *arg)
{
    const char *prog = arg;
    mime_spawn((char *const[]){ (char *)prog, NULL });
    return 1;
}

void build_system(struct widget *page)
{
    struct utsname u;
    char text[160];
    if (uname(&u) == 0) {
        snprintf(text, sizeof text, "%s %s (%s) on %s", u.sysname, u.release, u.version, u.machine);
        label_new(page, text);
    }
    snprintf(text, sizeof text, "%d processor%s", nproc(), nproc() == 1 ? "" : "s");
    label_new(page, text);
    struct gui_output_info info;
    if (gui_get_output(0, &info) == 0) {
        snprintf(text, sizeof text, "Display %dx%d pixels at scale %d", info.width * info.scale, info.height * info.scale,
                 info.scale);
        label_new(page, text);
    }
    uptime_label = label_new(page, "");
    mem_label = label_new(page, "");
    system_tick(NULL);
    app_timer_add(app, 1000, 1, system_tick, NULL);
    separator_new(page);
    label_new(page, "Tools");
    struct widget *row = box_new(page, 0);
    widget_connect(button_new(row, "System monitor"), "clicked", on_launch, "/bin/sysmon");
    widget_connect(button_new(row, "Kernel log"), "clicked", on_launch, "/bin/logview");
    widget_connect(button_new(row, "X12 settings"), "clicked", on_launch, "/bin/x12settings");
}
