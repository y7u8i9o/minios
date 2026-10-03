/* logview: the kernel log from /dev/klog in a table that follows new lines.
 *
 *   logview [-l debug|info|warning|error] [-s SUBSYSTEM] [-f TEXT]
 *
 * Every line of the form "[seconds] [L subsystem] message" is a row with
 * its time, level, subsystem and message, and a dot in the colour of the
 * level. Lines without that form are rows of level Info without a
 * subsystem. The rows are filtered by the lowest level shown, by one
 * subsystem and by a text that the line contains, in any case. The
 * options set the filters at the start. The pane below the table shows
 * the selected line in full. Follow scrolls to each new row. Copy puts the
 * selected line on the clipboard, Save writes the rows shown to a file,
 * and Clear removes the rows read so far from the window. The status bar
 * counts the rows, the warnings and errors, and the bytes that the kernel
 * ring dropped after the start before logview read them. */
#include <minios/conf.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gui/app.h>
#include <gui/model.h>
#include <gui/i18n.h>
#include <minios/input.h>

#define MAX_ENTRIES 10000       /* the oldest rows are dropped beyond this */
#define MAX_SUBSYS 64
#define LINE_MAX_LEN 1024

enum { L_DEBUG, L_INFO, L_WARNING, L_ERROR, NLEVELS };
static const char *const level_names[NLEVELS] = { N_("Debug"), N_("Info"), N_("Warning"), N_("Error") };
static const char *const level_args[NLEVELS] = { "debug", "info", "warning", "error" };
static const uint32_t level_colors[NLEVELS] = { 0xff9a9a9a, 0xff3a7bd5, 0xffe08a1e, 0xffd03b3b };

struct entry {
    char *raw;                  /* the whole line */
    long us;                    /* time since boot, -1 without one */
    int level;
    char subsys[16];
    const char *msg;            /* inside raw */
};

static struct app *app;
static struct widget *win, *table, *detail, *level_combo, *subsys_combo, *search, *follow_box;
static struct widget *st_rows, *st_levels, *st_lost;

static struct entry entries[MAX_ENTRIES];
static long first_seq, next_seq;        /* sequence numbers of the stored rows */
static long *shown;                     /* sequence numbers that pass the filters */
static long nshown, cap_shown;
static long warnings, errors, lost;

static char subsystems[MAX_SUBSYS][16];
static int nsubsys;
static int min_level = L_DEBUG, subsys_filter = -1;   /* -1: every subsystem */
static char needle[128];
static const char *want_subsys;         /* -s, applied when the subsystem appears */

static int klog_fd;
static char partial[LINE_MAX_LEN];
static size_t npartial;
static int skip_to_newline;             /* after a loss, the first bytes end a line already dropped */
static int have_read;                   /* bytes dropped before the first read are not counted */

static struct image *dots[NLEVELS];

static struct entry *entry_of(long seq)
{
    return seq >= first_seq && seq < next_seq ? &entries[seq % MAX_ENTRIES] : NULL;
}

/* ---- parsing ---- */

static void parse(struct entry *e)
{
    const char *s = e->raw;
    e->us = -1;
    e->level = L_INFO;
    e->subsys[0] = '\0';
    e->msg = s;
    unsigned long sec, usec;
    int n = 0;
    if (sscanf(s, "[%lu.%lu]%n", &sec, &usec, &n) != 2 || n == 0)
        return;
    e->us = (long)(sec * 1000000 + usec);
    const char *p = s + n;
    while (*p == ' ')
        p++;
    e->msg = p;
    if (p[0] != '[' || !p[1] || p[2] != ' ')
        return;
    const char *close = strchr(p, ']');
    if (!close)
        return;
    switch (p[1]) {
    case 'D': e->level = L_DEBUG; break;
    case 'W': e->level = L_WARNING; break;
    case 'E': e->level = L_ERROR; break;
    default: e->level = L_INFO; break;
    }
    size_t len = (size_t)(close - (p + 3));
    if (len >= sizeof e->subsys)
        len = sizeof e->subsys - 1;
    memcpy(e->subsys, p + 3, len);
    e->subsys[len] = '\0';
    p = close + 1;
    while (*p == ' ')
        p++;
    e->msg = p;
}

static int contains_nocase(const char *hay, const char *pat)
{
    if (!pat[0])
        return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (pat[i] && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)pat[i]))
            i++;
        if (!pat[i])
            return 1;
    }
    return 0;
}

static int passes(const struct entry *e)
{
    if (e->level < min_level)
        return 0;
    if (subsys_filter >= 0 && strcmp(e->subsys, subsystems[subsys_filter]) != 0)
        return 0;
    return contains_nocase(e->raw, needle);
}

/* ---- the model ---- */

static int m_rows(struct model *m, int parent) { return parent < 0 ? (int)nshown : 0; }
static int m_child(struct model *m, int parent, int index) { return (int)shown[index]; }
static int m_columns(struct model *m) { return 4; }

static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    const struct entry *e = entry_of(row);
    if (!e)
        return "";
    switch (col) {
    case 0:
        if (e->us < 0)
            return "";
        snprintf(buf, size, "%ld%s%06ld", e->us / 1000000, nl_langinfo(RADIXCHAR), e->us % 1000000);
        return buf;
    case 1: return _(level_names[e->level]);
    case 2: return e->subsys;
    default: return e->msg;
    }
}

static const char *m_header(struct model *m, int col)
{
    static const char *const names[] = { N_("Time"), N_("Level"), N_("Subsystem"), N_("Message") };
    return _(names[col]);
}

static const struct image *m_icon(struct model *m, int row)
{
    const struct entry *e = entry_of(row);
    return e ? dots[e->level] : NULL;
}

static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL, m_icon };

/* A dot of the level's colour, drawn before the time. */
static struct image *make_dot(uint32_t color)
{
    struct image *img = image_create(10, 10);
    if (!img)
        return NULL;
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 10; x++) {
            int dx = 2 * x - 9, dy = 2 * y - 9;
            if (dx * dx + dy * dy <= 64)
                img->pixels[y * 10 + x] = color;
        }
    return img;
}

/* ---- rows ---- */

static void update_status(void)
{
    char text[96], warning_text[48], error_text[48];
    long total = next_seq - first_seq;
    snprintf(text, sizeof text, ngettext("%ld of %ld line", "%ld of %ld lines", (unsigned long)total), nshown, total);
    widget_set_text(st_rows, text);
    snprintf(warning_text, sizeof warning_text, ngettext("%ld warning", "%ld warnings", (unsigned long)warnings),
             warnings);
    snprintf(error_text, sizeof error_text, ngettext("%ld error", "%ld errors", (unsigned long)errors), errors);
    snprintf(text, sizeof text, _("%s, %s"), warning_text, error_text);
    widget_set_text(st_levels, text);
    if (lost) {
        snprintf(text, sizeof text, ngettext("%ld byte lost", "%ld bytes lost", (unsigned long)lost), lost);
        widget_set_text(st_lost, text);
    } else {
        widget_set_text(st_lost, "");
    }
}

static void push_shown(long seq)
{
    if (nshown == cap_shown) {
        cap_shown = cap_shown ? cap_shown * 2 : 1024;
        shown = realloc(shown, (size_t)cap_shown * sizeof *shown);
    }
    shown[nshown++] = seq;
}

static void scroll_to_end(void)
{
    if (follow_box->value && nshown)
        view_scroll_to(table, (int)shown[nshown - 1]);
}

static void refilter(void)
{
    nshown = 0;
    for (long seq = first_seq; seq < next_seq; seq++)
        if (passes(entry_of(seq)))
            push_shown(seq);
    view_refresh(table);
    scroll_to_end();
    update_status();
}

static void drop_oldest(void)
{
    struct entry *e = entry_of(first_seq);
    if (e->level == L_WARNING) warnings--;
    if (e->level == L_ERROR) errors--;
    free(e->raw);
    e->raw = NULL;
    if (nshown && shown[0] == first_seq) {
        memmove(shown, shown + 1, (size_t)(nshown - 1) * sizeof *shown);
        nshown--;
    }
    first_seq++;
}

static void note_subsystem(const char *name)
{
    if (!name[0])
        return;
    for (int i = 0; i < nsubsys; i++)
        if (strcmp(subsystems[i], name) == 0)
            return;
    if (nsubsys == MAX_SUBSYS)
        return;
    strlcpy(subsystems[nsubsys++], name, sizeof subsystems[0]);
    combobox_add(subsys_combo, name);
    if (want_subsys && strcmp(want_subsys, name) == 0)
        combobox_select(subsys_combo, nsubsys);
}

static int add_line(const char *text, size_t len)
{
    if (next_seq - first_seq == MAX_ENTRIES)
        drop_oldest();
    struct entry *e = &entries[next_seq % MAX_ENTRIES];
    e->raw = malloc(len + 1);
    if (!e->raw)
        return 0;
    memcpy(e->raw, text, len);
    e->raw[len] = '\0';
    parse(e);
    if (e->level == L_WARNING) warnings++;
    if (e->level == L_ERROR) errors++;
    note_subsystem(e->subsys);
    long seq = next_seq++;
    if (passes(e)) {
        push_shown(seq);
        return 1;
    }
    return 0;
}

/* ---- reading ---- */

static void on_klog(int fd, int revents, void *arg)
{
    char buf[512];
    int added = 0;
    for (;;) {
        long before = lseek(fd, 0, SEEK_CUR);
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        long after = lseek(fd, 0, SEEK_CUR);
        if (before >= 0 && after >= 0 && after - before > n) {
            if (have_read)
                lost += after - before - n;
            npartial = 0;
            skip_to_newline = 1;
        }
        have_read = 1;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (skip_to_newline) {
                skip_to_newline = c != '\n';
                continue;
            }
            if (c == '\n') {
                added += add_line(partial, npartial);
                npartial = 0;
            } else if (npartial < sizeof partial - 1) {
                partial[npartial++] = c;
            }
        }
    }
    if (added)
        view_refresh(table);
    scroll_to_end();
    update_status();
}

/* ---- commands ---- */

static void message(const char *text)
{
    const char *const buttons[] = { _("OK") };
    app_dialog(app, _("Kernel log"), text, buttons, 1);
}

static int on_select(struct widget *w, void *args, void *arg)
{
    const struct entry *e = entry_of(((struct sig_select *)args)->index);
    editor_set_text(detail, e ? e->raw : "");
    return 0;
}

static int on_level(struct widget *w, void *args, void *arg)
{
    min_level = ((struct sig_select *)args)->index;
    refilter();
    return 1;
}

static int on_subsys(struct widget *w, void *args, void *arg)
{
    subsys_filter = ((struct sig_select *)args)->index - 1;
    refilter();
    return 1;
}

static int on_search(struct widget *w, void *args, void *arg)
{
    strlcpy(needle, widget_text(w), sizeof needle);
    refilter();
    return 0;
}

static int on_follow(struct widget *w, void *args, void *arg)
{
    scroll_to_end();
    return 0;
}

static int on_copy(struct widget *w, void *args, void *arg)
{
    const struct entry *e = entry_of(table->value);
    if (e)
        gui_clipboard_set(e->raw, (int)strlen(e->raw));
    return 1;
}

static int on_save(struct widget *w, void *args, void *arg)
{
    char name[PATH_MAX];
    snprintf(name, sizeof name, "%s/klog.txt", conf_home());
    if (!app_choose_file(app, FILE_CHOOSER_SAVE, _("Save"), NULL, 0, name, sizeof name))
        return 1;
    FILE *f = fopen(name, "w");
    if (!f) {
        char text[PATH_MAX + 64];
        snprintf(text, sizeof text, _("%s cannot be written: %s"), name, strerror(errno));
        message(text);
        return 1;
    }
    for (long i = 0; i < nshown; i++)
        fprintf(f, "%s\n", entry_of(shown[i])->raw);
    int err = ferror(f);
    if (fclose(f) != 0 || err) {
        message(_("The file was not written completely."));
        return 1;
    }
    printf("logview: saved %ld lines to %s\n", nshown, name);
    fflush(stdout);
    return 1;
}

static int on_clear(struct widget *w, void *args, void *arg)
{
    while (first_seq < next_seq)
        drop_oldest();
    nshown = 0;
    editor_set_text(detail, "");
    view_refresh(table);
    update_status();
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg) { app_quit(app, 0); return 1; }

static void usage(void)
{
    fprintf(stderr, "usage: logview [-l debug|info|warning|error] [-s SUBSYSTEM] [-f TEXT]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            int found = -1;
            for (int l = 0; l < NLEVELS; l++)
                if (strcmp(argv[i + 1], level_args[l]) == 0)
                    found = l;
            if (found < 0)
                usage();
            min_level = found;
            i++;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            want_subsys = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            strlcpy(needle, argv[++i], sizeof needle);
        } else {
            usage();
        }
    }
    app = app_create();
    if (!app)
        return 1;
    textdomain("logview");
    klog_fd = open("/dev/klog", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (klog_fd < 0) {
        perror("logview: /dev/klog");
        return 1;
    }
    for (int l = 0; l < NLEVELS; l++)
        dots[l] = make_dot(level_colors[l]);
    win = app_window(app, 820, 500, _("Kernel log"));
    if (!win)
        return 1;
    struct widget *mb = menubar_new(win);
    struct widget *file = menu_new(mb, _("File"));
    struct widget *m = menu_add(file, _("Save..."), "save");
    widget_connect(m, "clicked", on_save, NULL);
    widget_set_accel(m, KEY_S, WMOD_CTRL);
    menu_add_separator(file);
    widget_connect(menu_add(file, _("Quit"), "quit"), "clicked", on_quit, NULL);
    struct widget *edit = menu_new(mb, _("Edit"));
    m = menu_add(edit, _("Copy"), "copy");
    widget_connect(m, "clicked", on_copy, NULL);
    widget_set_accel(m, KEY_C, WMOD_CTRL);
    widget_connect(menu_add(edit, _("Clear"), NULL), "clicked", on_clear, NULL);

    struct widget *tools = toolbar_new(win);
    widget_connect(toolbar_add(tools, "save", _("Save")), "clicked", on_save, NULL);
    widget_connect(toolbar_add(tools, "copy", _("Copy")), "clicked", on_copy, NULL);
    level_combo = combobox_new(tools);
    combobox_add(level_combo, _("All levels"));
    combobox_add(level_combo, _("Info and above"));
    combobox_add(level_combo, _("Warnings and errors"));
    combobox_add(level_combo, _("Errors"));
    combobox_select(level_combo, min_level);
    widget_connect(level_combo, "changed", on_level, NULL);
    subsys_combo = combobox_new(tools);
    combobox_add(subsys_combo, _("All subsystems"));
    combobox_select(subsys_combo, 0);
    widget_connect(subsys_combo, "changed", on_subsys, NULL);
    search = textfield_new(tools, needle);
    widget_set_hint(search, 180, 0);
    widget_set_max(search, 180, 0);
    widget_set_tip(search, _("Find"));
    widget_connect(search, "changed", on_search, NULL);
    follow_box = checkbox_new(tools, _("Follow"));
    widget_set_value(follow_box, 1);
    widget_connect(follow_box, "toggled", on_follow, NULL);

    struct widget *split = splitpane_new(win, 1);
    widget_set_stretch(split, 1, 1);
    table = table_new(split);
    view_set_model(table, &model);
    table_set_column_width(table, 0, 110);
    table_set_column_width(table, 1, 70);
    table_set_column_width(table, 2, 90);
    table_set_column_width(table, 3, 520);
    widget_connect(table, "selected", on_select, NULL);
    detail = editor_new(split);
    editor_set_readonly(detail, 1);
    editor_set_wrap(detail, 1);
    splitpane_set_position(split, 340);

    struct widget *sb = statusbar_new(win);
    st_rows = statusbar_add(sb, 1);
    st_levels = statusbar_add(sb, 0);
    widget_set_min(st_levels, 160, 0);
    st_lost = statusbar_add(sb, 0);
    widget_set_min(st_lost, 110, 0);

    app_watch_fd(app, klog_fd, POLLIN, on_klog, NULL);
    on_klog(klog_fd, POLLIN, NULL);
    widget_focus(table);
    app_run(app);
    app_destroy(app);
    return 0;
}
