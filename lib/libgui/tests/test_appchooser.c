/* The application chooser over launcher and handler tables in temporary
 * folders, driven through its window.  The cases cover the recommended
 * handlers before the other applications, package handlers that the user
 * table overrides, commands with arguments, entries of the panel and
 * duplicates left out, the check box Always use, a program added through
 * Other program, and cancelling. */
#include <gui/app.h>
#include <gui/mime.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <minios/local.h>
#include "../src/appchooser.h"
#include "check.h"

struct app *app_create_detached(void);

static char root[64];

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void key(struct widget *win, int code, int ch)
{
    struct wmsg m = { .type = WM_KEY, .window = window_state_of(win)->win->id, .a = code, .b = 1, .d = ch };
    window_message(win, &m);
    m.b = 0;
    window_message(win, &m);
}

static void click(struct widget *win, const char *id)
{
    struct sig_click c = { 1, 0, 0 };
    widget_emit(widget_find(win, id), "clicked", &c);
}

static int rows(struct widget *win)
{
    return view_visible_rows(widget_find(win, "ac-tree"));
}

static int selected(struct widget *win)
{
    return widget_find(win, "ac-tree")->value;
}

static void file_text(const char *path, char *buf, size_t size)
{
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(buf, 1, size - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
}

void run_appchooser_tests(void)
{
    strcpy(root, "/tmp/minios_ac.XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0, "mkdtemp");
        return;
    }
    mkdir(PKG_DB, 0755);
    write_file(PKG_LAUNCHER, "# packages\nText editor=/usr/bin/edit\nViewer=/usr/bin/view\n"
                             "Player=/usr/bin/player\nScreenshot=/bin/screenshot -i\n"
                             "Log out=@logout\nViewer=/usr/bin/view\n");
    write_file(PKG_MIME_APPS, "text/plain /usr/bin/edit\nimage/* /usr/bin/view\n");
    char types[128], apps[128], notes[128], image[128], data[128], out[256], text[512];
    snprintf(types, sizeof types, "%s/mime.types", root);
    snprintf(apps, sizeof apps, "%s/mime.apps", root);
    snprintf(notes, sizeof notes, "%s/notes.txt", root);
    snprintf(image, sizeof image, "%s/a.png", root);
    snprintf(data, sizeof data, "%s/data.bin", root);
    write_file(types, "text/plain txt\nimage/png png\n");
    write_file(apps, "text/plain /usr/bin/gedit -n\n");
    write_file(notes, "x");
    write_file(image, "x");
    write_file(data, "x");
    CHECK(mime_load(types, apps) == 0, "mime_load");

    /* The user table overrides the package handler, which mime_handlers still lists. */
    const char *list[8];
    CHECK(strcmp(mime_handler("text/plain"), "/usr/bin/gedit -n") == 0, "handler with an argument: %s",
          mime_handler("text/plain"));
    int n = mime_handlers("text/plain", list, 8);
    CHECK(n == 2 && strcmp(list[0], "/usr/bin/gedit -n") == 0 && strcmp(list[1], "/usr/bin/edit") == 0,
          "two text handlers, the user's first: %d", n);
    n = mime_handlers("image/png", list, 8);
    CHECK(n == 1 && strcmp(list[0], "/usr/bin/view") == 0, "image/png through image/*: %d", n);

    struct app *a = app_create_detached();

    /* Text: gedit -n and Text editor recommended, Player, Screenshot and
     * Viewer under the other applications, Log out and the second Viewer
     * left out.  The default handler is selected. */
    struct appchooser *c = appchooser_open(a, NULL, notes);
    struct widget *win = appchooser_window(c);
    window_paint(win);
    CHECK(rows(win) == 7, "two headings and five applications: %d", rows(win));
    CHECK(selected(win) == 2, "the default handler is selected: %d", selected(win));
    CHECK(widget_find(win, "ac-accept")->enabled, "Open enabled with a selection");
    key(win, KEY_DOWN, 0);
    key(win, KEY_ENTER, '\n');
    CHECK(appchooser_state(c, out, sizeof out) == 1 && strcmp(out, "/usr/bin/edit") == 0,
          "Enter chooses the package editor: %s", out);
    appchooser_close(c);

    /* Image: the heading row disables Open.  Always use writes the user table. */
    c = appchooser_open(a, NULL, image);
    win = appchooser_window(c);
    window_paint(win);
    CHECK(rows(win) == 6, "one recommended and three other applications: %d", rows(win));
    key(win, KEY_DOWN, 0);
    CHECK(!widget_find(win, "ac-accept")->enabled, "Open disabled on a heading");
    key(win, KEY_DOWN, 0);
    widget_find(win, "ac-always")->value = 1;
    click(win, "ac-accept");
    CHECK(appchooser_state(c, out, sizeof out) == 1 && strcmp(out, "/usr/bin/player") == 0,
          "Open chooses Player: %s", out);
    appchooser_close(c);
    file_text(apps, text, sizeof text);
    CHECK(strstr(text, "image/png /usr/bin/player\n") != NULL, "the user table has the new handler: %s", text);
    CHECK(strstr(text, "text/plain /usr/bin/gedit -n\n") != NULL, "the user table retains its entry: %s", text);
    CHECK(strcmp(mime_handler("image/png"), "/usr/bin/player") == 0, "image/png opens with Player");

    /* No handler: only the other applications, nothing selected.  Other
     * program adds a program once, at the top, and refuses a data file. */
    c = appchooser_open(a, NULL, data);
    win = appchooser_window(c);
    window_paint(win);
    CHECK(rows(win) == 5, "one heading and four applications: %d", rows(win));
    CHECK(!widget_find(win, "ac-accept")->enabled, "Open disabled without a selection");
    CHECK(appchooser_add_program(c, notes) == -ENOEXEC, "a data file is not a program");
    CHECK(appchooser_add_program(c, "/bin/sh") == 0, "/bin/sh is a program");
    CHECK(appchooser_add_program(c, "/bin/sh") == 0, "/bin/sh a second time");
    window_paint(win);
    CHECK(rows(win) == 6, "the program is added once: %d", rows(win));
    CHECK(selected(win) == 2, "the program is at the top and selected: %d", selected(win));
    key(win, KEY_ENTER, '\n');
    CHECK(appchooser_state(c, out, sizeof out) == 1 && strcmp(out, "/bin/sh") == 0, "Enter chooses /bin/sh: %s", out);
    appchooser_close(c);

    /* Escape cancels. */
    c = appchooser_open(a, NULL, notes);
    win = appchooser_window(c);
    window_paint(win);
    key(win, KEY_ESC, 27);
    CHECK(appchooser_state(c, out, sizeof out) == -1, "Escape cancels");
    appchooser_close(c);

    remove(PKG_LAUNCHER);
    remove(PKG_MIME_APPS);
    rmdir(PKG_DB);
    remove(types);
    remove(apps);
    remove(notes);
    remove(image);
    remove(data);
    rmdir(root);
}
