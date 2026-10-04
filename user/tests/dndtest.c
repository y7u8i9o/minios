/* dndtest: the client of the gui_dnd boot test, a window of 240x160
 * logical pixels filled by a canvas.
 *
 *   dndtest source FILE...   each drag from the canvas carries the next
 *                            file as text/uri-list and text/plain; the
 *                            files are created when missing
 *   dndtest target FOLDER    files dropped on the canvas are copied or
 *                            moved into the folder, which is created
 *
 * Both log on standard output with the prefix "dndtest: ". */
#include <gui/app.h>
#include <gui/fileops.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char **files;
static int nfiles, next_file;
static int press_x, press_y, pressed;
static const char *folder;
static int peeked;

static const char *action_name(int a) { return a == GUI_DND_MOVE ? "move" : a == GUI_DND_COPY ? "copy" : "none"; }

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    press_x = c->x;
    press_y = c->y;
    pressed = (c->button & 1) && next_file < nfiles;
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    if (!pressed || !(c->button & 1) || !widget_drag_moved(press_x, press_y, c->x, c->y))
        return 1;
    pressed = 0;
    const char *path = files[next_file];
    const char *paths[1] = { path };
    char *uris = fileops_uri_list(paths, 1);
    struct gui_drag_item items[2] = { { "text/uri-list", uris, strlen(uris) }, { "text/plain", path, strlen(path) } };
    const char *slash = strrchr(path, '/');
    int r = widget_drag_start(w, items, 2, GUI_DND_COPY | GUI_DND_MOVE, NULL, slash ? slash + 1 : path);
    printf("dndtest: drag of %s %s\n", path, r == 0 ? "started" : "refused");
    fflush(stdout);
    free(uris);
    return 1;
}

static int on_release(struct widget *w, void *args, void *arg)
{
    pressed = 0;
    return 1;
}

static int on_drag_end(struct widget *w, void *args, void *arg)
{
    struct sig_drag *sd = args;
    printf("dndtest: drag of %s ended, action %s\n", files[next_file], action_name(sd->drag->action));
    fflush(stdout);
    next_file++;
    return 1;
}

static int on_drag_motion(struct widget *w, void *args, void *arg)
{
    struct sig_drag *sd = args;
    if (!widget_drag_offers("text/uri-list"))
        return 1;
    size_t len;
    const char *uris = gui_drag_peek("text/uri-list", &len);
    int preferred = fileops_drop_action(uris, len, folder);
    if (uris && !peeked) {
        char **paths;
        int n = fileops_parse_uri_list(uris, len, &paths);
        printf("dndtest: peeked %d paths, preferred %s\n", n, action_name(preferred));
        fflush(stdout);
        fileops_free_paths(paths, n);
        peeked = 1;
    }
    if (preferred) {
        sd->drag->accept_mime = "text/uri-list";
        sd->drag->accept_actions = GUI_DND_COPY | GUI_DND_MOVE;
        sd->drag->preferred = preferred;
    }
    return 1;
}

static int on_drop(struct widget *w, void *args, void *arg)
{
    struct sig_drag *sd = args;
    char **paths;
    int n = fileops_parse_drop(sd->drag->mime, sd->drag->data, sd->drag->len, &paths);
    const char *failed;
    int r = fileops_drop(paths, n, folder, sd->drag->action, &failed);
    printf("dndtest: dropped %d files, action %s\n", r, action_name(sd->drag->action));
    fflush(stdout);
    fileops_free_paths(paths, n);
    peeked = 0;
    return 1;
}

static int on_drag_leave(struct widget *w, void *args, void *arg)
{
    peeked = 0;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3 || (strcmp(argv[1], "source") != 0 && strcmp(argv[1], "target") != 0)) {
        fprintf(stderr, "usage: dndtest source FILE... | dndtest target FOLDER\n");
        return 2;
    }
    int source = strcmp(argv[1], "source") == 0;
    if (source) {
        files = argv + 2;
        nfiles = argc - 2;
        for (int i = 0; i < nfiles; i++) {
            struct stat st;
            FILE *f = stat(files[i], &st) == 0 ? NULL : fopen(files[i], "w");
            if (f) {
                fprintf(f, "dropped file %d\n", i + 1);
                fclose(f);
            }
        }
    } else {
        folder = argv[2];
        mkdir(folder, 0755);
    }
    struct app *app = app_create();
    if (!app) {
        fprintf(stderr, "dndtest: no display\n");
        return 1;
    }
    struct widget *win = app_window(app, 240, 160, source ? "dnd source" : "dnd target");
    widget_set_padding(win, 0);
    struct widget *canvas = canvas_new(win);
    widget_set_stretch(canvas, 1, 1);
    if (source) {
        widget_connect(canvas, "press", on_press, NULL);
        widget_connect(canvas, "motion", on_motion, NULL);
        widget_connect(canvas, "release", on_release, NULL);
        widget_connect(canvas, "drag_end", on_drag_end, NULL);
    } else {
        widget_connect(canvas, "drag_motion", on_drag_motion, NULL);
        widget_connect(canvas, "drop", on_drop, NULL);
        widget_connect(canvas, "drag_leave", on_drag_leave, NULL);
    }
    printf("dndtest: %s ready\n", argv[1]);
    fflush(stdout);
    int code = app_run(app);
    app_destroy(app);
    return code;
}
