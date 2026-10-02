/* The Region and language page (L7, docs/design/desktop.md): the language
 * and the formats of the session, the time zone, the keyboard layout, and
 * the input methods (I5, docs/design/ime.md): the engines of imed and
 * their order, the switch keys and the candidate window.
 * The language and the formats are written to the configuration file as
 * lang and formats, which startgui and /etc/profile export as LANG and the
 * format categories.  They apply to the programs started afterwards.  The
 * time zone is the target of the symbolic link /etc/localtime, which every
 * program reads again when it changes. */
#include "settings.h"
#include <dirent.h>
#include <locale.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOCALE_DIR "/usr/share/i18n/locales"
#define ZONE_DIR "/usr/share/zoneinfo"
#define LOCALTIME "/etc/localtime"
#define MAX_LOCALES 32
#define MAX_ZONES 96

static char locales[MAX_LOCALES][32];
static char titles[MAX_LOCALES][128];   /* "Français (France)" for each locale */
static int nlocales;
static char zones[MAX_ZONES][48];
static int nzones;
static int building;
static struct widget *lang_combo, *formats_combo, *zone_combo, *sample;

/* The engines of imed that the page knows, in the order of ime_engines,
 * the disabled ones last. */
static const struct { const char *name, *title; } engines[] = {
    { "pinyin", N_("Chinese (Pinyin)") },
    { "japanese", N_("Japanese") },
};
#define NENGINES ((int)(sizeof engines / sizeof engines[0]))
static int order[NENGINES], enabled[NENGINES];
static struct widget *engine_check[NENGINES];

/* locale_title reads the language and territory names of a locale file,
 * "Français (France)" for fr_FR. */
static void locale_title(const char *name, char *out, size_t size)
{
    char path[96], line[160], language[64] = "", territory[64] = "";
    snprintf(path, sizeof path, "%s/%s", LOCALE_DIR, name);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *value = strchr(line, '"'), *end = value ? strrchr(value + 1, '"') : NULL;
        if (!end)
            continue;
        *end = '\0';
        if (strncmp(line, "language_name ", 14) == 0)
            strlcpy(language, value + 1, sizeof language);
        else if (strncmp(line, "territory_name ", 15) == 0)
            strlcpy(territory, value + 1, sizeof territory);
    }
    if (f)
        fclose(f);
    if (language[0] && territory[0])
        snprintf(out, size, "%s (%s)", language, territory);
    else
        strlcpy(out, name, size);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

static void read_locales(void)
{
    DIR *d = opendir(LOCALE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && nlocales < MAX_LOCALES)
        if (e->d_name[0] != '.')
            strlcpy(locales[nlocales++], e->d_name, sizeof locales[0]);
    if (d)
        closedir(d);
    qsort(locales, (size_t)nlocales, sizeof locales[0], by_name);
    for (int i = 0; i < nlocales; i++)
        locale_title(locales[i], titles[i], sizeof titles[i]);
}

/* The zones of zones.tab, in its order: the first field of each line. */
static void read_zones(void)
{
    FILE *f = fopen(ZONE_DIR "/zones.tab", "r");
    char line[160];
    while (f && fgets(line, sizeof line, f) && nzones < MAX_ZONES) {
        if (line[0] == '#')
            continue;
        line[strcspn(line, "\t\n")] = '\0';
        if (line[0])
            strlcpy(zones[nzones++], line, sizeof zones[0]);
    }
    if (f)
        fclose(f);
}

/* locale_index finds a setting such as "fr_FR.UTF-8" among the locales. */
static int locale_index(const char *value)
{
    size_t n = strcspn(value, ".@");
    for (int i = 0; i < nlocales; i++)
        if (strlen(locales[i]) == n && strncmp(locales[i], value, n) == 0)
            return i;
    return -1;
}

/* current_zone reads the zone that /etc/localtime names, or UTC. */
static int current_zone(void)
{
    char target[160];
    ssize_t n = readlink(LOCALTIME, target, sizeof target - 1);
    if (n > 0) {
        target[n] = '\0';
        const char *name = strncmp(target, ZONE_DIR "/", sizeof ZONE_DIR) == 0 ? target + sizeof ZONE_DIR : target;
        for (int i = 0; i < nzones; i++)
            if (strcmp(zones[i], name) == 0)
                return i;
    }
    for (int i = 0; i < nzones; i++)
        if (strcmp(zones[i], "UTC") == 0)
            return i;
    return 0;
}

/* show_sample formats the current time and a number in the formats that
 * the page selects, in the time zone of /etc/localtime. */
static void show_sample(void)
{
    int f = formats_combo->value > 0 ? formats_combo->value - 1 : lang_combo->value;
    char name[48], when[96] = "", number[48] = "", text[200];
    snprintf(name, sizeof name, "%s.UTF-8", f >= 0 && f < nlocales ? locales[f] : "C");
    locale_t loc = newlocale(LC_ALL_MASK, name, (locale_t)0);
    locale_t old = loc ? uselocale(loc) : (locale_t)0;
    time_t now = time(NULL);
    struct tm tm;
    tzset();
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%c", &tm);
    snprintf(number, sizeof number, "%'.2f", 1234567.89);
    if (loc) {
        uselocale(old);
        freelocale(loc);
    }
    snprintf(text, sizeof text, _("Example: %s, %s"), when, number);
    widget_set_text(sample, text);
}

static int on_lang(struct widget *w, void *args, void *arg)
{
    if (building || w->value < 0 || w->value >= nlocales)
        return 1;
    char value[48];
    snprintf(value, sizeof value, "%s.UTF-8", locales[w->value]);
    conf_set("lang", value);
    show_sample();
    return 1;
}

static int on_formats(struct widget *w, void *args, void *arg)
{
    if (building || w->value < 0)
        return 1;
    char value[48] = "";
    if (w->value > 0 && w->value <= nlocales)
        snprintf(value, sizeof value, "%s.UTF-8", locales[w->value - 1]);
    conf_set("formats", value);
    show_sample();
    return 1;
}

/* A new zone replaces the link /etc/localtime. */
static int on_zone(struct widget *w, void *args, void *arg)
{
    if (building || w->value < 0 || w->value >= nzones)
        return 1;
    char target[96];
    snprintf(target, sizeof target, "%s/%s", ZONE_DIR, zones[w->value]);
    unlink(LOCALTIME);
    if (symlink(target, LOCALTIME) < 0) {
        const char *buttons[] = { _("OK") };
        app_dialog(app, _("Region and language"), _("The time zone cannot be set."), buttons, 1);
        return 1;
    }
    printf("settings: time zone %s\n", zones[w->value]);
    fflush(stdout);
    show_sample();
    return 1;
}

static void read_engines(void)
{
    char list[128];
    strlcpy(list, conf_get("ime_engines"), sizeof list);
    int n = 0;
    for (char *name = strtok(list, ", "); name; name = strtok(NULL, ", "))
        for (int i = 0; i < NENGINES; i++)
            if (strcmp(engines[i].name, name) == 0 && !enabled[i]) {
                enabled[i] = 1;
                order[n++] = i;
            }
    for (int i = 0; i < NENGINES; i++)
        if (!enabled[i])
            order[n++] = i;
}

static void write_engines(void)
{
    char list[128] = "";
    for (int k = 0; k < NENGINES; k++)
        if (enabled[order[k]]) {
            if (list[0])
                strlcat(list, ",", sizeof list);
            strlcat(list, engines[order[k]].name, sizeof list);
        }
    conf_set("ime_engines", list);
}

static void show_engines(void)
{
    building = 1;
    for (int k = 0; k < NENGINES; k++) {
        widget_set_text(engine_check[k], _(engines[order[k]].title));
        engine_check[k]->value = enabled[order[k]];
        widget_invalidate(engine_check[k]);
    }
    building = 0;
}

static int on_engine(struct widget *w, void *args, void *arg)
{
    if (building)
        return 1;
    int k = (int)(intptr_t)arg;
    enabled[order[k]] = w->value != 0;
    write_engines();
    return 1;
}

/* Move up exchanges an engine with the one before it. */
static int on_engine_up(struct widget *w, void *args, void *arg)
{
    int k = (int)(intptr_t)arg;
    if (k <= 0)
        return 1;
    int t = order[k - 1];
    order[k - 1] = order[k];
    order[k] = t;
    show_engines();
    write_engines();
    return 1;
}

static int on_switch_key(struct widget *w, void *args, void *arg)
{
    if (!building)
        conf_set(arg, w->value ? "1" : "0");
    return 1;
}

static int on_page_size(struct widget *w, void *args, void *arg)
{
    if (!building)
        conf_set_int("ime_page_size", w->value);
    return 1;
}

static int on_orientation(struct widget *w, void *args, void *arg)
{
    if (!building && w->value >= 0)
        conf_set("ime_orientation", w->value == 1 ? "vertical" : "horizontal");
    return 1;
}

/* build_input_methods adds the input method section to the grid. */
static int build_input_methods(struct widget *grid, int r)
{
    read_engines();
    struct widget *heading = label_new(grid, _("Input methods"));
    widget_set_grid(heading, r++, 0, 1, 2);
    for (int k = 0; k < NENGINES; k++) {
        engine_check[k] = checkbox_new(grid, "");
        widget_set_grid(engine_check[k], r, 0, 1, 1);
        widget_connect(engine_check[k], "toggled", on_engine, (void *)(intptr_t)k);
        if (k > 0) {
            struct widget *up = button_new(grid, _("Move up"));
            widget_set_grid(up, r, 1, 1, 1);
            widget_set_align(up, ALIGN_START, ALIGN_CENTER);
            widget_connect(up, "clicked", on_engine_up, (void *)(intptr_t)k);
        }
        r++;
    }
    struct widget *shift = checkbox_new(grid, _("A Shift tap toggles the input method"));
    shift->value = conf_int("ime_shift_toggle", 1) != 0;
    widget_connect(shift, "toggled", on_switch_key, "ime_shift_toggle");
    widget_set_grid(shift, r++, 0, 1, 2);
    struct widget *ctrl = checkbox_new(grid, _("Ctrl+Space toggles the input method"));
    ctrl->value = conf_int("ime_ctrl_space", 1) != 0;
    widget_connect(ctrl, "toggled", on_switch_key, "ime_ctrl_space");
    widget_set_grid(ctrl, r++, 0, 1, 2);
    row_label(grid, r, _("Candidates per page"));
    struct widget *size = spinner_new(grid, 2, 9, conf_int("ime_page_size", 5));
    widget_connect(size, "changed", on_page_size, NULL);
    widget_set_grid(size, r++, 1, 1, 1);
    row_label(grid, r, _("Candidate layout"));
    struct widget *layout = combobox_new(grid);
    combobox_add(layout, _("Horizontal"));
    combobox_add(layout, _("Vertical"));
    combobox_select(layout, strcmp(conf_get("ime_orientation"), "vertical") == 0 ? 1 : 0);
    widget_connect(layout, "changed", on_orientation, NULL);
    widget_set_grid(layout, r++, 1, 1, 1);
    show_engines();
    return r;
}

static void sample_tick(void *arg)
{
    show_sample();
}

void build_region(struct widget *page)
{
    building = 1;
    read_locales();
    read_zones();
    struct widget *grid = grid_new(page);
    widget_set_stretch(grid, 1, 0);
    grid_set_stretch(grid, -1, 1, 1);
    int r = 0;

    row_label(grid, r, _("Language"));
    lang_combo = combobox_new(grid);
    for (int i = 0; i < nlocales; i++)
        combobox_add(lang_combo, titles[i]);
    int lang = locale_index(conf_get("lang"));
    if (lang < 0)
        lang = locale_index("en_US");
    combobox_select(lang_combo, lang < 0 ? 0 : lang);
    widget_connect(lang_combo, "changed", on_lang, NULL);
    widget_set_grid(lang_combo, r++, 1, 1, 1);

    row_label(grid, r, _("Formats"));
    formats_combo = combobox_new(grid);
    combobox_add(formats_combo, _("Same as the language"));
    for (int i = 0; i < nlocales; i++)
        combobox_add(formats_combo, titles[i]);
    int formats = conf_get("formats")[0] ? locale_index(conf_get("formats")) : -1;
    combobox_select(formats_combo, formats + 1);
    widget_connect(formats_combo, "changed", on_formats, NULL);
    widget_set_grid(formats_combo, r++, 1, 1, 1);

    row_label(grid, r, _("Time zone"));
    zone_combo = combobox_new(grid);
    for (int i = 0; i < nzones; i++)
        combobox_add(zone_combo, zones[i]);
    combobox_select(zone_combo, current_zone());
    widget_connect(zone_combo, "changed", on_zone, NULL);
    widget_set_grid(zone_combo, r++, 1, 1, 1);

    row_label(grid, r, _("Keyboard layout"));
    widget_set_grid(layout_combo_new(grid), r++, 1, 1, 1);
    sample = label_new(grid, "");
    widget_set_grid(sample, r++, 0, 1, 2);
    r = build_input_methods(grid, r);
    (void)r;
    show_sample();
    app_timer_add(app, 1000, 1, sample_tick, NULL);
    building = 0;
}
