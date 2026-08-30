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
