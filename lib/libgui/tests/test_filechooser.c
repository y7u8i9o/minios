/* The file chooser over a temporary tree, driven through its window.  The
 * cases cover the initial folder and selection, filters, the path bar's
 * memory of the folders below, search, the location entry with
 * completion, save names, hidden files, the recent list and cancelling. */
#include <gui/app.h>
#include <gui/mime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/filechooser.h"
#include "check.h"

struct app *app_create_detached(void);

static char root[64];

static void touch(const char *rel)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("x", f);
        fclose(f);
    }
}

static void folder(const char *rel)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    mkdir(path, 0755);
}

static void key(struct widget *win, int code, int ch, int mods)
{
    struct wmsg m = { .type = WM_KEY, .window = window_state_of(win)->win->id, .a = code, .b = 1, .c = mods, .d = ch };
    window_message(win, &m);
    m.b = 0;
    window_message(win, &m);
}

static void type(struct widget *win, const char *s)
{
    for (; *s; s++)
        key(win, 0, *s, 0);
}

static void enter(struct widget *win) { key(win, KEY_ENTER, '\n', 0); }

static int rows(struct widget *win)
{
    return view_visible_rows(widget_find(win, "fv-table"));
}

static void path_of(char *out, size_t size, const char *rel)
{
    snprintf(out, size, "%s%s%s", root, rel[0] ? "/" : "", rel);
}

void run_filechooser_tests(void)
{
    strcpy(root, "/tmp/minios_fc.XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0, "mkdtemp");
        return;
    }
    char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", root, 1);
    char types[128];
    snprintf(types, sizeof types, "%s/mime.types", root);
    FILE *f = fopen(types, "w");
    fprintf(f, "image/png png\ntext/plain txt\n");
    fclose(f);
    mime_load(types, "/nonexistent");
    folder("Pictures");
    folder("Pictures/sub");
    folder("Music");
    touch("Pictures/a.png");
    touch("Pictures/b.txt");
    touch("Pictures/sub/deep.png");
    touch("notes.txt");
    touch(".hidden");

    struct app *a = app_create_detached();
    char path[256], out[256];
    static const struct file_filter images[] = { { "Images", "image/*" }, { "All files", "*" } };

    /* Opened at a file, the chooser shows its folder, folders first, with the file selected. */
    path_of(path, sizeof path, "Pictures/a.png");
    struct chooser *c = chooser_open(a, NULL, FILE_CHOOSER_OPEN, "Open", NULL, 0, path);
    struct widget *win = chooser_window(c);
    window_paint(win);
    struct widget *table = widget_find(win, "fv-table");
    CHECK(rows(win) == 3, "three entries in Pictures: %d", rows(win));
    CHECK(table->value == 1, "a.png selected after the folder sub: %d", table->value);
    CHECK(widget_find(win, "fc-accept")->enabled, "Open enabled with a selection");
    enter(win);
    CHECK(chooser_state(c, out, sizeof out) == 1 && strcmp(out, path) == 0, "Enter opens %s: %s", path, out);
    chooser_close(c);

    /* A MIME filter hides the text file, which the second filter shows. */
    c = chooser_open(a, NULL, FILE_CHOOSER_OPEN, NULL, images, 2, path);
    win = chooser_window(c);
    window_paint(win);
    CHECK(rows(win) == 2, "the image filter leaves sub and a.png: %d", rows(win));
    combobox_select(widget_find(win, "fc-filter"), 1);
    CHECK(rows(win) == 3, "all files: %d", rows(win));
    key(win, KEY_ESC, 27, 0);
    CHECK(chooser_state(c, out, sizeof out) == -1, "Escape cancels");
    chooser_close(c);

    /* The path bar retains the folders below, so Alt+Down returns after Alt+Up. */
    path_of(path, sizeof path, "Pictures/sub/");
    c = chooser_open(a, NULL, FILE_CHOOSER_OPEN, NULL, NULL, 0, path);
    win = chooser_window(c);
    window_paint(win);
    table = widget_find(win, "fv-table");
    CHECK(rows(win) == 1, "sub contains deep.png: %d", rows(win));
    key(win, KEY_UP, 0, WMOD_ALT);
    key(win, KEY_UP, 0, WMOD_ALT);
    CHECK(rows(win) == 4, "Alt+Up twice reaches the home folder: %d", rows(win));
    CHECK(table->value == 1, "the folder just left is selected: %d", table->value);
    key(win, KEY_DOWN, 0, WMOD_ALT);
    key(win, KEY_DOWN, 0, WMOD_ALT);
    CHECK(rows(win) == 1, "Alt+Down twice returns to sub: %d", rows(win));

    /* Typing over the table searches below the folder. */
    key(win, KEY_UP, 0, WMOD_ALT);
    key(win, KEY_UP, 0, WMOD_ALT);
    widget_focus(table);
    type(win, "dee");
    struct widget *search = widget_find(win, "fv-search");
    CHECK(search->visible && strcmp(widget_text(search), "dee") == 0, "search field shows '%s'", widget_text(search));
    CHECK(rows(win) == 1, "one result for dee: %d", rows(win));
    key(win, KEY_ESC, 27, 0);
    CHECK(!search->visible && rows(win) == 4, "Escape ends the search: %d rows", rows(win));

    /* Ctrl+L and inline completion.  P completes Pictures/ and Enter opens
     * it, then a completes a.png and Enter chooses it. */
    key(win, KEY_L, 12, WMOD_CTRL);
    struct widget *loc = widget_find(win, "fv-location");
    path_of(path, sizeof path, "");
    strcat(path, "/");
    CHECK(loc->visible && strcmp(widget_text(loc), path) == 0, "location starts at the folder: '%s'", widget_text(loc));
    type(win, "P");
    path_of(path, sizeof path, "Pictures/");
    CHECK(strcmp(widget_text(loc), path) == 0, "P completes to '%s'", widget_text(loc));
    type(win, "ic");
    CHECK(strcmp(widget_text(loc), path) == 0, "typing over the completion retains it: '%s'", widget_text(loc));
    enter(win);
    CHECK(!loc->visible && rows(win) == 3, "Enter opens Pictures: %d rows", rows(win));
    key(win, KEY_L, 12, WMOD_CTRL);
    type(win, "a");
    path_of(path, sizeof path, "Pictures/a.png");
    CHECK(strcmp(widget_text(loc), path) == 0, "a completes to '%s'", widget_text(loc));
    enter(win);
    CHECK(chooser_state(c, out, sizeof out) == 1 && strcmp(out, path) == 0, "the location opens %s", out);
    chooser_close(c);

    /* Saving starts with the name of the initial path, Enter on a folder
     * name enters the folder, and a new name is chosen. */
    path_of(path, sizeof path, "new.txt");
    c = chooser_open(a, NULL, FILE_CHOOSER_SAVE, NULL, NULL, 0, path);
    win = chooser_window(c);
    window_paint(win);
    struct widget *name = widget_find(win, "fc-name");
    CHECK(strcmp(widget_text(name), "new.txt") == 0, "name field '%s'", widget_text(name));
    CHECK(widget_focused(win) == name, "the name field has the focus");
    key(win, 0, 1, WMOD_CTRL);         /* Ctrl+A */
    type(win, "Music");
    enter(win);
    CHECK(chooser_state(c, NULL, 0) == 0 && rows(win) == 0, "a folder name enters the folder: %d rows", rows(win));
    CHECK(widget_text(name)[0] == '\0', "the name field is emptied");
    CHECK(!widget_find(win, "fc-accept")->enabled, "Save disabled without a name");
    type(win, "song.wav");
    enter(win);
    path_of(path, sizeof path, "Music/song.wav");
    CHECK(chooser_state(c, out, sizeof out) == 1 && strcmp(out, path) == 0, "saves as %s", out);
    chooser_close(c);
    touch("Music/song.wav");            /* what the program would write */

    /* Selecting a file names it, and Ctrl+H shows hidden files. */
    path_of(path, sizeof path, "");
    c = chooser_open(a, NULL, FILE_CHOOSER_SAVE, NULL, NULL, 0, path);
    win = chooser_window(c);
    window_paint(win);
    table = widget_find(win, "fv-table");
    name = widget_find(win, "fc-name");
    int before = rows(win);
    widget_focus(table);
    key(win, KEY_H, 8, WMOD_CTRL);
    CHECK(rows(win) == before + 1 + 1, "hidden files: %d after %d", rows(win), before);  /* .hidden, .local */
    view_select(table, rows(win) - 1);
    CHECK(strcmp(widget_text(name), "notes.txt") == 0, "the selected file names the save: '%s'", widget_text(name));

    /* Recent lists the files chosen above, newest first. */
    struct widget *places = widget_find(win, "fv-sidebar");
    int ax, ay;
    widget_abs(places, &ax, &ay);
    struct wmsg m = { .type = WM_MOUSE, .window = window_state_of(win)->win->id, .a = ax + 20, .b = ay + 10,
                      .c = 1, .d = WMOUSE_DOWN };
    window_message(win, &m);
    m.c = 0;
    m.d = WMOUSE_UP;
    window_message(win, &m);
    CHECK(rows(win) == 2, "Recent contains a.png and song.wav: %d", rows(win));
    window_message(win, &(struct wmsg){ .type = WM_CLOSE, .window = window_state_of(win)->win->id });
    CHECK(chooser_state(c, out, sizeof out) == -1, "closing the window cancels");
    chooser_close(c);

    char recent[256];
    snprintf(recent, sizeof recent, "%s/.local/share/recent-files", root);
    f = fopen(recent, "r");
    char first[256] = "";
    if (f) {
        if (!fgets(first, sizeof first, f))
            first[0] = '\0';
        fclose(f);
    }
    path_of(path, sizeof path, "Music/song.wav\n");
    CHECK(strcmp(first, path) == 0, "newest recent entry '%s'", first);

    app_step(a, 0);
    app_destroy(a);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0)
        CHECK(0, "remove %s", root);
    if (saved_home) {
        setenv("HOME", saved_home, 1);
        free(saved_home);
    } else {
        unsetenv("HOME");
    }
}
