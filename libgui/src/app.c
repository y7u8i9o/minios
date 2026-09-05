/* The application object: event loop over the server queue, timers and
 * watched descriptors; window registry; theme. */
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef MINIOS_HOST
#include <poll.h>
long uptime_ms(void);
#else
#include <sys/ipc.h>
#endif

struct timer {
    long deadline;
    int interval;
    int repeat;
    timer_fn fn;
    void *arg;
    struct timer *next;
};

struct watch {
    int fd, events;
    watch_fn fn;
    void *arg;
    struct watch *next;
};

struct app {
    struct theme theme;
    struct widget **windows;
    int nwindows;
    struct timer *timers;
    struct watch *watches;
    int quit, code;
    int damage_log;
    int connected;
};

struct app *app_create(void)
{
    struct app *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    if (gui_connect() < 0) {
        free(a);
        return NULL;
    }
    a->connected = 1;
    theme_init_default(&a->theme);
    theme_apply(&a->theme);
    return a;
}

/* For the host tests: the fake client of tests/fake_client.c stands in
 * for the server, and the builtin font replaces the outline font. */
struct app *app_create_detached(void);
struct app *app_create_detached(void)
{
    struct app *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->connected = gui_connect() == 0;
    theme_init_default(&a->theme);
    a->theme.font_path[0] = '\0';
    theme_apply(&a->theme);
    return a;
}

void app_destroy(struct app *a)
{
    for (int i = 0; i < a->nwindows; i++)
        widget_destroy(a->windows[i]);
    free(a->windows);
    for (struct timer *t = a->timers; t;) {
        struct timer *n = t->next;
        free(t);
        t = n;
    }
    for (struct watch *w = a->watches; w;) {
        struct watch *n = w->next;
        free(w);
        w = n;
    }
    theme_release(&a->theme);
    if (a->connected)
        gui_disconnect();
    free(a);
}

struct theme *app_theme(struct app *a)
{
    return &a->theme;
}

void app_theme_changed(struct app *a)
{
    theme_apply(&a->theme);
    for (int i = 0; i < a->nwindows; i++)
        widget_relayout(a->windows[i]);
}

static struct widget *register_window(struct app *a, struct widget *w, struct gui_window *win, const char *title)
{
    struct window_state *ws = window_state_of(w);
    if (a->connected) {
        if (!win) {
            widget_destroy(w);
            return NULL;
        }
        ws->win = win;
        w->w = win->width;
        w->h = win->height;
    }
    widget_set_text(w, title);
    struct widget **grown = realloc(a->windows, (size_t)(a->nwindows + 1) * sizeof *grown);
    if (!grown) {
        widget_destroy(w);
        return NULL;
    }
    a->windows = grown;
    a->windows[a->nwindows++] = w;
    return w;
}

struct widget *app_window(struct app *a, int width, int height, const char *title)
{
    struct widget *w = widget_new(&window_class, NULL);
    if (!w)
        return NULL;
    w->app = a;
    w->window = w;
    w->w = width;
    w->h = height;
    return register_window(a, w, a->connected ? gui_create_window(width, height, title) : NULL, title);
}

struct widget *app_modal_window(struct app *a, struct widget *parent, int width, int height, const char *title)
{
    struct widget *w = widget_new(&window_class, NULL);
    if (!w)
        return NULL;
    w->app = a;
    w->window = w;
    w->w = width;
    w->h = height;
    struct gui_window *pw = parent ? window_state_of(parent)->win : NULL;
    return register_window(a, w, a->connected ? gui_create_dialog_window(pw, width, height, title) : NULL, title);
}

struct widget *app_layer_window(struct app *a, int width, int height, int layer, int anchor, int exclusive,
                                int keyboard, const char *ns)
{
    struct widget *w = widget_new(&window_class, NULL);
    if (!w)
        return NULL;
    w->app = a;
    w->window = w;
    w->w = width;
    w->h = height;
    return register_window(a, w, a->connected ? gui_create_layer_window(width, height, layer, anchor, exclusive,
                                                                        keyboard, ns) : NULL, ns);
}

struct widget *app_first_window(struct app *a)
{
    return a->nwindows ? a->windows[0] : NULL;
}

struct widget *app_next_window(struct app *a, struct widget *window)
{
    for (int i = 0; i + 1 < a->nwindows; i++)
        if (a->windows[i] == window)
            return a->windows[i + 1];
    return NULL;
}

static struct widget *window_by_id(struct app *a, int id)
{
    for (int i = 0; i < a->nwindows; i++) {
        struct window_state *ws = window_state_of(a->windows[i]);
        if (window_owns_id(a->windows[i], id))
            return a->windows[i];
    }
    return NULL;
}

void app_dispatch(struct app *a, struct wmsg *m)
{
    struct widget *w = window_by_id(a, m->window);
    if (w)
        window_message(w, m);
}

/* ---- timers and watches ---- */

struct timer *app_timer_add(struct app *a, int ms, int repeat, timer_fn fn, void *arg)
{
    struct timer *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->deadline = uptime_ms() + ms;
    t->interval = ms;
    t->repeat = repeat;
    t->fn = fn;
    t->arg = arg;
    t->next = a->timers;
    a->timers = t;
    return t;
}

void app_timer_remove(struct app *a, struct timer *t)
{
    for (struct timer **pp = &a->timers; *pp; pp = &(*pp)->next)
        if (*pp == t) {
            *pp = t->next;
            free(t);
            return;
        }
}

struct watch *app_watch_fd(struct app *a, int fd, int events, watch_fn fn, void *arg)
{
    struct watch *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->fd = fd;
    w->events = events;
    w->fn = fn;
    w->arg = arg;
    w->next = a->watches;
    a->watches = w;
    return w;
}

void app_unwatch_fd(struct app *a, struct watch *w)
{
    for (struct watch **pp = &a->watches; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            free(w);
            return;
        }
}

static void run_timers(struct app *a)
{
    long now = uptime_ms();
    for (struct timer *t = a->timers; t;) {
        struct timer *next = t->next;
        if (t->deadline <= now) {
            if (t->repeat)
                t->deadline = now + t->interval;
            timer_fn fn = t->fn;
            void *arg = t->arg;
            if (!t->repeat)
                app_timer_remove(a, t);
            fn(arg);
        }
        t = next;
    }
}

/* ---- painting and closed windows ---- */

void app_set_damage_log(struct app *a, int on)
{
    a->damage_log = on;
}

static void paint_all(struct app *a)
{
    for (int i = 0; i < a->nwindows; i++) {
        struct rect r = window_paint(a->windows[i]);
        if (a->damage_log && !rect_empty(r)) {
            printf("app: damage %d,%d %dx%d\n", r.x, r.y, r.w, r.h);
            fflush(stdout);
        }
    }
    if (a->connected)
        gui_flush();
}

static void reap_windows(struct app *a)
{
    for (int i = 0; i < a->nwindows;) {
        if (window_state_of(a->windows[i])->closed) {
            widget_destroy(a->windows[i]);
            memmove(&a->windows[i], &a->windows[i + 1], (size_t)(a->nwindows - i - 1) * sizeof *a->windows);
            a->nwindows--;
        } else {
            i++;
        }
    }
}

int app_step(struct app *a, int timeout_ms)
{
    paint_all(a);
    long now = uptime_ms();
    int wait = timeout_ms;
    for (struct timer *t = a->timers; t; t = t->next) {
        long d = t->deadline - now;
        if (d < 0) d = 0;
        if (wait < 0 || d < wait)
            wait = (int)d;
    }
    int rt = a->connected ? gui_repeat_timeout() : -1;
    if (rt >= 0 && (wait < 0 || rt < wait))
        wait = rt;
    int nw = 1;
    for (struct watch *w = a->watches; w; w = w->next)
        nw++;
    struct pollfd *pf = calloc((size_t)nw, sizeof *pf);
    pf[0].fd = a->connected ? gui_event_fd() : -1;
    pf[0].events = POLLIN;
    int i = 1;
    for (struct watch *w = a->watches; w; w = w->next, i++) {
        pf[i].fd = w->fd;
        pf[i].events = (short)w->events;
    }
    int r = poll(pf, (unsigned)nw, wait);
    if (r < 0 && errno != EINTR) {
        free(pf);
        return 0;
    }
    if (r > 0) {
        i = 1;
        for (struct watch *w = a->watches; w; i++) {
            struct watch *next = w->next;
            if (pf[i].revents)
                w->fn(w->fd, pf[i].revents, w->arg);
            w = next;
        }
    }
    free(pf);
    if (a->connected) {
        struct wmsg m;
        while (gui_next_event(&m, 0) == 1)
            app_dispatch(a, &m);
    }
    run_timers(a);
    reap_windows(a);
    paint_all(a);
    return !a->quit && a->nwindows > 0;
}

int app_run(struct app *a)
{
    while (app_step(a, -1))
        ;
    return a->code;
}

void app_quit(struct app *a, int code)
{
    a->quit = 1;
    a->code = code;
}
