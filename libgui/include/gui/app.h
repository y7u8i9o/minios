#pragma once
/* The application object: one loop over the server queue, timers and
 * watched descriptors, driving every window of the process. */
#include <gui/widget.h>

struct app;
struct timer;
struct watch;

typedef void (*timer_fn)(void *arg);
typedef void (*watch_fn)(int fd, int revents, void *arg);

struct app *app_create(void);           /* connects to the server; NULL on failure */
void app_destroy(struct app *a);
struct theme *app_theme(struct app *a);
/* Apply theme changes (font, scale) to every window. */
void app_theme_changed(struct app *a);
struct widget *app_window(struct app *a, int width, int height, const char *title);
struct widget *app_modal_window(struct app *a, struct widget *parent, int width, int height, const char *title);
/* A window on a layer surface (see gui_create_layer_window); it has no
 * decorations and its size follows the compositor's configure. */
struct widget *app_layer_window(struct app *a, int width, int height, int layer, int anchor, int exclusive,
                                int keyboard, const char *ns);
/* Run until app_quit or the last window closes; returns the exit code. */
int app_run(struct app *a);
void app_quit(struct app *a, int code);
/* One iteration: handle pending messages, timers and descriptors, then
 * paint. timeout_ms < 0 blocks. Returns 0 when the loop should stop. */
int app_step(struct app *a, int timeout_ms);
struct timer *app_timer_add(struct app *a, int ms, int repeat, timer_fn fn, void *arg);
void app_timer_remove(struct app *a, struct timer *t);
struct watch *app_watch_fd(struct app *a, int fd, int events, watch_fn fn, void *arg);
void app_unwatch_fd(struct app *a, struct watch *w);
/* Windows registered with the application, in creation order. */
struct widget *app_first_window(struct app *a);
struct widget *app_next_window(struct app *a, struct widget *window);
/* Deliver a server message (tests). */
void app_dispatch(struct app *a, struct wmsg *m);
/* Called by windows after painting; the application logs damage when
 * app_set_damage_log is on (tests). */
void app_set_damage_log(struct app *a, int on);
/* Modal dialogs (src/dialog.c): app_dialog returns the chosen button
 * index or -1; app_prompt edits buf and returns 1 for OK. */
int app_dialog(struct app *a, const char *title, const char *text, const char *const *buttons, int nbuttons);
int app_prompt(struct app *a, const char *title, const char *label, char *buf, int size);

/* The file chooser (src/filechooser.c).  path holds the initial file or
 * folder, empty for the home folder, and receives the chosen path of
 * size bytes.  The result is 1 when a file was chosen and 0 when the
 * window was cancelled.  A save chooser asks before an existing file is
 * replaced.  title may be NULL.  Each filter names a set of patterns
 * separated by spaces.  A pattern with a slash matches the MIME type
 * (image/ followed by an asterisk matches every image), the others match
 * the file name ("*.png").  The first
 * filter is selected, and without filters every file is shown. */
enum file_chooser_mode { FILE_CHOOSER_OPEN, FILE_CHOOSER_SAVE };
struct file_filter {
    const char *name;
    const char *patterns;
};
int app_choose_file(struct app *a, enum file_chooser_mode mode, const char *title,
                    const struct file_filter *filters, int nfilters, char *path, int size);
