/* Drag and drop on the host: the text/uri-list helpers and the drops of
 * gui/fileops.h over a temporary tree, and the framework's drags through
 * window messages with the fake client: rows of the folder view dragged
 * out and files dropped on a folder row, text dragged out of the editor
 * and moved inside it, text dropped into the editor, and text moved out
 * of a text field. */
#include <gui/app.h>
#include <gui/fileops.h>
#include <gui/folderview.h>
#include <gui/model.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"
#include "fake.h"

struct app *app_create_detached(void);

static char root[64];

static void path_of(char *out, size_t size, const char *rel)
{
    snprintf(out, size, "%s/%s", root, rel);
}

static void touch(const char *rel)
{
    char path[256];
    path_of(path, sizeof path, rel);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("data", f);
        fclose(f);
    }
}

static int exists(const char *rel)
{
    char path[256];
    struct stat st;
    path_of(path, sizeof path, rel);
    return lstat(path, &st) == 0;
}

static char *uri_of(const char *rel)
{
    char path[256];
    path_of(path, sizeof path, rel);
    const char *paths[1] = { path };
    return fileops_uri_list(paths, 1);
}

static void drag_msg(struct widget *win, uint32_t type, int x, int y, int action, int actions)
{
    struct wmsg m = { .type = type, .window = window_state_of(win)->win->id, .a = x, .b = y, .c = action, .d = actions };
    window_message(win, &m);
}

static void mouse(struct widget *win, int kind, int x, int y, int buttons)
{
    struct wmsg m = { .type = WM_MOUSE, .window = window_state_of(win)->win->id, .a = x, .b = y, .c = buttons, .d = kind };
    window_message(win, &m);
}

/* Press at (x, y) and move far enough to start a drag. */
static void press_and_move(struct widget *win, int x, int y)
{
    mouse(win, WMOUSE_DOWN, x, y, 1);
    mouse(win, WMOUSE_MOVE, x + 3, y, 1);
    mouse(win, WMOUSE_MOVE, x + DRAG_THRESHOLD + 4, y + 2, 1);
}

static void reset_fake(void)
{
    memset(&fake_drag, 0, sizeof fake_drag);
    memset(fake_offers, 0, sizeof fake_offers);
    fake_peek = fake_drop = fake_drop_mime = fake_accept_mime = NULL;
    fake_accept_actions = fake_accept_preferred = 0;
}

static void test_uri_list(void)
{
    const char *paths[2] = { "/home/a b/x%y.txt", "/tmp/plain" };
    char *list = fileops_uri_list(paths, 2);
    CHECK(list && strcmp(list, "file:///home/a%20b/x%25y.txt\r\nfile:///tmp/plain\r\n") == 0, "uri list: '%s'", list);
    char **out;
    int n = fileops_parse_uri_list(list, strlen(list), &out);
    CHECK(n == 2 && strcmp(out[0], paths[0]) == 0 && strcmp(out[1], paths[1]) == 0, "parsed back: %d", n);
    fileops_free_paths(out, n);
    free(list);
    const char *mixed = "# a comment\nhttp://example.org/x\nfile://localhost/etc/passwd\nfile:///bin/sh";
    n = fileops_parse_uri_list(mixed, strlen(mixed), &out);
    CHECK(n == 2 && strcmp(out[0], "/etc/passwd") == 0 && strcmp(out[1], "/bin/sh") == 0, "comments and other schemes skipped: %d", n);
    fileops_free_paths(out, n);
}

static void test_drops(void)
{
    char box[256], here[256];
    path_of(box, sizeof box, "box");
    mkdir(box, 0755);
    touch("one.txt");
    char *uris = uri_of("one.txt");
    CHECK(fileops_drop_action(uris, strlen(uris), box) == GUI_DND_MOVE, "one file system: move preferred");
    CHECK(fileops_drop_action(uris, strlen(uris), root) == 0, "into its own folder: refused");
    CHECK(fileops_drop_action(NULL, 0, box) == GUI_DND_COPY, "unknown paths: copy");
    char *box_uri = uri_of("box");
    path_of(here, sizeof here, "box");
    CHECK(fileops_drop_action(box_uri, strlen(box_uri), here) == 0, "a folder into itself: refused");

    char **paths;
    int n = fileops_parse_uri_list(uris, strlen(uris), &paths);
    const char *failed;
    CHECK(fileops_drop(paths, n, box, GUI_DND_COPY, &failed) == 1 && exists("box/one.txt") && exists("one.txt"),
          "copied into the folder");
    CHECK(fileops_drop(paths, n, box, GUI_DND_COPY, &failed) == -EEXIST && failed == paths[0], "an existing name is not replaced");
    CHECK(fileops_drop(paths, n, root, GUI_DND_COPY, &failed) == 1 && exists("Copy of one.txt"), "a copy into its own folder");
    CHECK(fileops_drop(paths, n, root, GUI_DND_MOVE, &failed) == 0 && exists("one.txt"), "a move into its own folder does nothing");
    fileops_free_paths(paths, n);
    n = fileops_parse_uri_list(box_uri, strlen(box_uri), &paths);
    CHECK(fileops_drop(paths, n, here, GUI_DND_COPY, &failed) == -EINVAL, "a folder is not copied into itself");
    fileops_free_paths(paths, n);
    free(uris);
    free(box_uri);
}

/* The centre of a row of the folder view's table, in window coordinates. */
static void row_point(struct widget *table, int index, int *x, int *y)
{
    int ax, ay;
    struct rect r = { 0 };
    widget_abs(table, &ax, &ay);
    view_row_rect(table, view_row_at(table, index), &r);
    *x = ax + 40;
    *y = ay + r.y + r.h / 2;
}

static void test_folderview(struct app *a)
{
    touch("two.txt");
    struct widget *win = app_window(a, 700, 400, "files");
    struct widget *bar = box_new(win, 0);
    struct widget *split = splitpane_new(win, 0);
    widget_set_stretch(split, 1, 1);
    struct folderview *fv = folderview_new(bar, split, "test", NULL, NULL);
    folderview_navigate(fv, root);
    window_paint(win);
    struct widget *table = folderview_table(fv);
    /* Rows: box, then Copy of one.txt, one.txt and two.txt by name. */
    int bx, by, fx, fy;
    row_point(table, 0, &bx, &by);
    row_point(table, 3, &fx, &fy);

    reset_fake();
    press_and_move(win, fx, fy);
    char two[256];
    path_of(two, sizeof two, "two.txt");
    char *uris = uri_of("two.txt");
    CHECK(fake_drag.started == 1 && fake_drag.nitems == 2 && strcmp(fake_drag.mime[0], "text/uri-list") == 0 &&
          strcmp(fake_drag.data[0], uris) == 0 && strcmp(fake_drag.data[1], two) == 0,
          "a row dragged out as text/uri-list and text/plain: %d '%s'", fake_drag.started, fake_drag.data[0]);
    CHECK(fake_drag.actions == (GUI_DND_COPY | GUI_DND_MOVE) && fake_drag.icon, "copy and move, with an image");

    /* The drag comes back over the folder row and is dropped there. */
    fake_offers[0] = "text/uri-list";
    fake_offers[1] = "text/plain";
    fake_peek = uris;
    drag_msg(win, WM_DRAG_ENTER, bx, by, 0, GUI_DND_COPY | GUI_DND_MOVE);
    CHECK(fake_accept_mime && strcmp(fake_accept_mime, "text/uri-list") == 0 && fake_accept_preferred == GUI_DND_MOVE,
          "the folder row takes the files, move preferred: %s %d", fake_accept_mime ? fake_accept_mime : "none",
          fake_accept_preferred);
    drag_msg(win, WM_DRAG_MOTION, fx, fy, GUI_DND_MOVE, GUI_DND_COPY | GUI_DND_MOVE);
    CHECK(!fake_accept_mime, "dropping into the folder the file is in is refused");
    drag_msg(win, WM_DRAG_MOTION, bx, by, GUI_DND_MOVE, GUI_DND_COPY | GUI_DND_MOVE);
    fake_drop = uris;
    fake_drop_mime = "text/uri-list";
    drag_msg(win, WM_DROP, bx, by, GUI_DND_MOVE, 0);
    CHECK(exists("box/two.txt") && !exists("two.txt"), "the dropped file moved into the folder");
    struct wmsg end = { .type = WM_DRAG_END, .window = window_state_of(win)->win->id, .a = GUI_DND_MOVE };
    window_message(win, &end);
    CHECK(!window_state_of(win)->drag_source && !window_state_of(win)->capture, "the end of the drag releases the table");
    CHECK(folderview_count(fv) == 3, "the moved file left the listing: %d", folderview_count(fv));
    free(uris);
    window_close(win);
}

/* A point on a text position of an editor without line numbers. */
static void text_point(struct widget *ed, const char *line, int row, int col, int *x, int *y)
{
    const struct font *f = widget_theme(ed)->font;
    int ax, ay;
    widget_abs(ed, &ax, &ay);
    *x = ax + 1 + 4 + gfx_text_width_font(f, line, col);
    *y = ay + 1 + row * (f->height + 2) + (f->height + 2) / 2;
}

static void test_editor(struct app *a)
{
    struct widget *win = app_window(a, 400, 200, "editor");
    struct widget *ed = editor_new(win);
    window_paint(win);
    widget_focus(ed);
    editor_set_text(ed, "hello world");
    editor_goto(ed, 0, 0);
    CHECK(editor_find(ed, "world", 1), "select world");
    int x, y, x0, y0;
    text_point(ed, "hello world", 0, 8, &x, &y);
    reset_fake();
    press_and_move(win, x, y);
    CHECK(fake_drag.started == 1 && strcmp(fake_drag.mime[0], "text/plain") == 0 && strcmp(fake_drag.data[0], "world") == 0,
          "the selection dragged out: '%s'", fake_drag.data[0]);

    /* Dropped at the start of the same editor: the text moves there. */
    fake_offers[0] = "text/plain";
    text_point(ed, "hello world", 0, 0, &x0, &y0);
    drag_msg(win, WM_DRAG_MOTION, x0, y0, GUI_DND_MOVE, GUI_DND_COPY | GUI_DND_MOVE);
    CHECK(fake_accept_mime && strcmp(fake_accept_mime, "text/plain") == 0 && fake_accept_preferred == GUI_DND_MOVE,
          "the editor takes its own text, move preferred");
    drag_msg(win, WM_DRAG_MOTION, x, y, GUI_DND_MOVE, GUI_DND_COPY | GUI_DND_MOVE);
    CHECK(!fake_accept_mime, "a drop into the dragged text is refused");
    drag_msg(win, WM_DRAG_MOTION, x0, y0, GUI_DND_MOVE, GUI_DND_COPY | GUI_DND_MOVE);
    fake_drop = "world";
    fake_drop_mime = "text/plain";
    drag_msg(win, WM_DROP, x0, y0, GUI_DND_MOVE, 0);
    struct wmsg end = { .type = WM_DRAG_END, .window = window_state_of(win)->win->id, .a = GUI_DND_MOVE };
    window_message(win, &end);
    char *t = editor_text(ed);
    CHECK(strcmp(t, "worldhello ") == 0, "moved inside the editor: '%s'", t);
    free(t);
    CHECK(editor_undo(ed), "one undo step");
    t = editor_text(ed);
    CHECK(strcmp(t, "hello world") == 0, "the move undone at once: '%s'", t);
    free(t);

    /* Text from another program is inserted at the drop caret. */
    reset_fake();
    fake_offers[0] = "text/plain";
    text_point(ed, "hello world", 0, 5, &x, &y);
    drag_msg(win, WM_DRAG_MOTION, x, y, GUI_DND_COPY, GUI_DND_COPY);
    CHECK(fake_accept_preferred == GUI_DND_COPY, "text of another program: copy preferred");
    fake_drop = ",";
    fake_drop_mime = "text/plain";
    drag_msg(win, WM_DROP, x, y, GUI_DND_COPY, 0);
    t = editor_text(ed);
    CHECK(strcmp(t, "hello, world") == 0, "inserted at the drop caret: '%s'", t);
    free(t);

    /* Moved to another program: the editor loses the text. */
    editor_goto(ed, 0, 0);
    CHECK(editor_find(ed, "world", 1), "select world again");
    text_point(ed, "hello, world", 0, 9, &x, &y);
    reset_fake();
    press_and_move(win, x, y);
    CHECK(fake_drag.started == 1, "dragged again");
    window_message(win, &end);
    t = editor_text(ed);
    CHECK(strcmp(t, "hello, ") == 0, "moved elsewhere: '%s'", t);
    free(t);

    /* A click inside the selection without a move clears it. */
    editor_set_text(ed, "abc def");
    editor_goto(ed, 0, 0);
    editor_find(ed, "def", 1);
    text_point(ed, "abc def", 0, 5, &x, &y);
    mouse(win, WMOUSE_DOWN, x, y, 1);
    mouse(win, WMOUSE_UP, x, y, 0);
    int l, c;
    editor_cursor(ed, &l, &c);
    CHECK(!editor_has_selection(ed) && c == 5, "a click in the selection places the cursor: %d", c);
    window_close(win);
}

static void test_textfield(struct app *a)
{
    struct widget *win = app_window(a, 300, 80, "field");
    struct widget *f = textfield_new(win, "alpha beta");
    window_paint(win);
    widget_focus(f);
    textfield_select(f, 6, 10);
    int ax, ay;
    widget_abs(f, &ax, &ay);
    int x = ax + 4 + gfx_text_width_font(widget_theme(f)->font, "alpha be", -1), y = ay + f->h / 2;
    reset_fake();
    press_and_move(win, x, y);
    CHECK(fake_drag.started == 1 && strcmp(fake_drag.data[0], "beta") == 0, "the selection of a field dragged: '%s'",
          fake_drag.data[0]);
    struct wmsg end = { .type = WM_DRAG_END, .window = window_state_of(win)->win->id, .a = GUI_DND_MOVE };
    window_message(win, &end);
    CHECK(strcmp(widget_text(f), "alpha ") == 0, "moved out of the field: '%s'", widget_text(f));
    textfield_set_masked(f, 1);
    widget_set_text(f, "secret");
    textfield_select(f, 0, 6);
    reset_fake();
    press_and_move(win, ax + 10, y);
    CHECK(fake_drag.started == 0, "a masked field is never dragged");
    window_close(win);
}

void run_dnd_tests(void)
{
    strcpy(root, "/tmp/minios_dnd.XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0, "mkdtemp");
        return;
    }
    test_uri_list();
    test_drops();
    struct app *a = app_create_detached();
    test_folderview(a);
    test_editor(a);
    test_textfield(a);
    app_destroy(a);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    system(cmd);
}
