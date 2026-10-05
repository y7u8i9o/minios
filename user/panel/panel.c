/* panel: a layer surface at the bottom of the screen with the launcher
 * menu of launcher.c, one button per toplevel from the toplevel manager,
 * the label of the keyboard layout or input method, and a clock. Built
 * directly on libwire and the libgui painter; the buffers are rendered at
 * the output's scale. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/timerfd.h>
#include <time.h>
#include <gui/i18n.h>
#include "panel.h"
#include <minios/local.h>
#include <minios/conf.h>
#include "ime-client.h"

#define MAX_TASKS 16

struct task { struct wire_proxy *handle; char title[48]; int active, minimized; };

struct wire_display *display;
struct wire_proxy *compositor, *shm, *shell, *seat;
static struct wire_proxy *pointer, *manager;
struct canvas panel;
static struct wire_proxy *layer;
static struct task tasks[MAX_TASKS];
static int ntasks;
int screen_w = 1024, screen_h = 768;
static int px, py;                       /* pointer in the panel */
static struct wire_proxy *pointer_surface;
uint32_t press_serial;
static int layer_configured;
int output_scale = 1;
struct theme ui;
static char input_label[16];             /* from the seat: "EN", "FR", "あ", ... */

/* ---- canvases ---- */

void canvas_release_buffer(struct canvas *c)
{
    if (c->buffer)
        buffer_destroy(c->buffer);
    if (c->pool)
        shm_pool_destroy(c->pool);
    if (c->s.pixels)
        munmap(c->s.pixels, (size_t)c->s.width * c->s.height * 4);
    if (c->fd >= 0)
        close(c->fd);
    c->buffer = c->pool = NULL;
    c->s.pixels = NULL;
    c->fd = -1;
}

/* Allocate the buffer for lw by lh logical pixels at the output scale;
 * the surface retains its role. */
static int canvas_alloc(struct canvas *c, int w, int h)
{
    canvas_release_buffer(c);
    int scale = output_scale > 0 ? output_scale : 1;
    int dw = w * scale, dh = h * scale;
    size_t size = (size_t)dw * dh * 4;
    c->fd = memfd_create("panel", MFD_CLOEXEC);
    if (c->fd < 0 || ftruncate(c->fd, (long)size) < 0)
        return -1;
    c->s.pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
    if (c->s.pixels == MAP_FAILED) {
        c->s.pixels = NULL;
        return -1;
    }
    c->s.width = dw;
    c->s.height = dh;
    c->s.stride = dw;
    c->lw = w;
    c->lh = h;
    c->scale = scale;
    c->pool = shm_create_pool(shm, c->fd, (int32_t)size);
    c->buffer = shm_pool_create_buffer(c->pool, 0, dw, dh, dw * 4, 1);
    surface_set_buffer_scale(c->surface, scale);
    return 0;
}

int canvas_create(struct canvas *c, int w, int h)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->surface = compositor_create_surface(compositor);
    return canvas_alloc(c, w, h);
}

void canvas_commit(struct canvas *c)
{
    surface_attach(c->surface, c->buffer, 0, 0);
    surface_damage(c->surface, 0, 0, c->lw, c->lh);
    surface_commit(c->surface);
}

void canvas_painter(struct painter *p, struct canvas *c)
{
    painter_init_scaled(p, &c->s, &ui, c->scale);
}

/* ---- drawing ---- */

void panel_label(struct painter *p, int x, int y, int w, int h, const char *text, uint32_t color, int centre)
{
    int th = painter_text_height(p);
    int tw = painter_text_width(p, text, -1);
    int tx = centre && tw < w - 8 ? (w - tw) / 2 : 6;
    painter_push(p, x, y, w, h);
    painter_text(p, tx, (h - th) / 2, text, color);
    painter_pop(p);
}

void draw_panel(void)
{
    struct painter p;
    canvas_painter(&p, &panel);
    int w = panel.lw, h = panel.lh;
    painter_fill(&p, 0, 0, w, h, PANEL_BG);
    painter_fill(&p, 0, 0, w, 1, PANEL_LINE);
    painter_rounded(&p, 4, 4, MENU_BTN_W, h - 8, launcher_is_open() ? BUTTON_OPEN : BUTTON_BG, 0xffffffffu);
    panel_label(&p, 4, 4, MENU_BTN_W, h - 8, _("Menu"), PANEL_TEXT, 1);
    int limit = (w - CLOCK_W - MIXER_BTN_W - INPUT_W - MENU_BTN_W - 24) / (TASK_BTN_W + 4);
    for (int i = 0; i < ntasks && i < limit; i++) {
        int x = MENU_BTN_W + 12 + i * (TASK_BTN_W + 4);
        int active = tasks[i].active && !tasks[i].minimized;
        painter_rounded(&p, x, 4, TASK_BTN_W, h - 8, active ? BUTTON_ACTIVE : BUTTON_BG, 0xffffffffu);
        if (active)
            painter_fill(&p, x + 8, h - 6, TASK_BTN_W - 16, 2, ACCENT);
        panel_label(&p, x + 6, 4, TASK_BTN_W - 12, h - 8, tasks[i].title, tasks[i].minimized ? PANEL_TEXT_DIM : PANEL_TEXT, 0);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char t[16];
    snprintf(t, sizeof t, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    panel_label(&p, w - CLOCK_W, 0, CLOCK_W - 6, h, t, PANEL_TEXT, 1);
    mixer_draw_button(&p);
    if (input_label[0])
        panel_label(&p, imemenu_x(), 0, INPUT_W, h, input_label, PANEL_TEXT, 1);
    canvas_commit(&panel);
}

/* ---- pointer ---- */

static void on_enter(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *surface, int32_t x, int32_t y)
{
    pointer_surface = surface;
    px = wire_fixed_to_int(x);
    py = wire_fixed_to_int(y);
}
static void on_leave(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *surface)
{
    if (pointer_surface == surface)
        pointer_surface = NULL;
}
static void on_motion(void *user, struct wire_proxy *p, uint32_t time, int32_t x, int32_t y)
{
    px = wire_fixed_to_int(x);
    py = wire_fixed_to_int(y);
    if (mixer_owns(pointer_surface)) {
        mixer_pointer_motion(px, py);
        return;
    }
    if (launcher_is_surface(pointer_surface))
        launcher_pointer_motion(px, py);
}
static void on_button(void *user, struct wire_proxy *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    if (state == 1)
        press_serial = serial;
    if (mixer_owns(pointer_surface)) {
        mixer_pointer_button(button, state, px, py);
        return;
    }
    if (imemenu_owns(pointer_surface)) {
        imemenu_pointer_button(button, state, px, py);
        return;
    }
    if (launcher_is_surface(pointer_surface)) {
        launcher_pointer_button(button, state, px, py);
        return;
    }
    if (button != 1 || state != 1)
        return;
    if (pointer_surface != panel.surface)
        return;
    if (px >= 4 && px < 4 + MENU_BTN_W) {
        launcher_toggle();
        return;
    }
    if (px >= mixer_button_x() && px < mixer_button_x() + MIXER_BTN_W) {
        mixer_toggle();
        return;
    }
    /* The input method label opens the menu of the methods. */
    if (px >= imemenu_x() && px < imemenu_x() + INPUT_W) {
        imemenu_toggle();
        return;
    }
    int limit = (screen_w - CLOCK_W - MIXER_BTN_W - INPUT_W - MENU_BTN_W - 24) / (TASK_BTN_W + 4);
    for (int i = 0; i < ntasks && i < limit; i++) {
        int x = MENU_BTN_W + 12 + i * (TASK_BTN_W + 4);
        if (px >= x && px < x + TASK_BTN_W) {
            log_line("task click %s", tasks[i].title);
            if (tasks[i].minimized || !tasks[i].active)
                toplevel_handle_activate(tasks[i].handle, seat);
            else
                toplevel_handle_minimize(tasks[i].handle);
            return;
        }
    }
}
static void on_axis(void *user, struct wire_proxy *p, uint32_t time, uint32_t axis, int32_t value) {}
static void on_frame(void *user, struct wire_proxy *p) {}
static const struct pointer_listener pointer_events = { on_enter, on_leave, on_motion, on_button, on_axis, on_frame };

/* ---- toplevel manager ---- */

static struct task *task_of(struct wire_proxy *h)
{
    for (int i = 0; i < ntasks; i++)
        if (tasks[i].handle == h)
            return &tasks[i];
    return NULL;
}
static void on_handle_title(void *user, struct wire_proxy *h, const char *title)
{
    struct task *t = task_of(h);
    if (t) {
        strlcpy(t->title, title, sizeof t->title);
        draw_panel();
    }
}
static void on_handle_app_id(void *user, struct wire_proxy *h, const char *app_id) {}
static void on_handle_state(void *user, struct wire_proxy *h, const struct wire_array *states)
{
    struct task *t = task_of(h);
    if (!t)
        return;
    t->active = t->minimized = 0;
    const uint32_t *v = states->data;
    for (size_t i = 0; i < states->size / 4; i++) {
        if (v[i] == 2) t->active = 1;
        if (v[i] == 3) t->minimized = 1;
    }
    draw_panel();
}
static void on_handle_closed(void *user, struct wire_proxy *h)
{
    struct task *t = task_of(h);
    if (t) {
        toplevel_handle_destroy(h);
        memmove(t, t + 1, (size_t)(&tasks[ntasks] - t - 1) * sizeof *t);
        ntasks--;
        draw_panel();
    }
}
static const struct toplevel_handle_listener handle_events = { on_handle_title, on_handle_app_id, on_handle_state, on_handle_closed };

static void on_toplevel(void *user, struct wire_proxy *m, struct wire_proxy *handle)
{
    if (ntasks == MAX_TASKS || !handle)
        return;
    handle->obj.interface = &toplevel_handle_interface;
    tasks[ntasks].handle = handle;
    strcpy(tasks[ntasks].title, "");
    ntasks++;
    toplevel_handle_add_listener(handle, &handle_events, NULL);
    log_line("toplevel listed");
}
static const struct toplevel_manager_listener manager_events = { on_toplevel };

/* ---- setup ---- */

static void on_layer_configure(void *user, struct wire_proxy *l, uint32_t serial, int32_t w, int32_t h)
{
    layer_surface_ack_configure(l, serial);
    /* A new width (mode change) or scale needs a new buffer. */
    if (w > 0 && (w != panel.lw || output_scale != panel.scale)) {
        if (canvas_alloc(&panel, w, PANEL_H) < 0)
            exit(1);
        screen_w = w;
        if (layer_configured)
            draw_panel();
    }
    layer_configured = 1;
}
static void on_layer_closed(void *user, struct wire_proxy *l) { exit(0); }
static const struct layer_surface_listener layer_events = { on_layer_configure, on_layer_closed };

static void on_geometry(void *user, struct wire_proxy *o, int32_t x, int32_t y, int32_t w, int32_t h) { screen_w = w; screen_h = h; }
static void on_mode(void *user, struct wire_proxy *o, int32_t w, int32_t h, int32_t r) {}
/* A scale change comes with a layer configure, which reallocates the
 * buffer; committing here would race with that configure's serial. */
static void on_scale(void *user, struct wire_proxy *o, int32_t factor) { output_scale = factor > 0 ? factor : 1; }
static void on_output_done(void *user, struct wire_proxy *o) {}
static void on_transform(void *user, struct wire_proxy *o, uint32_t transform) {}
static const struct output_listener output_events = { on_geometry, on_mode, on_scale, on_transform, on_output_done };

/* The seat of version 2 reports the keyboard layout or input method that
 * Super+Space and Alt+Shift select (docs/design/ime.md). */
static void on_capabilities(void *user, struct wire_proxy *s, uint32_t caps) {}
static void on_seat_name(void *user, struct wire_proxy *s, const char *name) {}
static void on_input_method(void *user, struct wire_proxy *s, const char *label)
{
    strlcpy(input_label, label, sizeof input_label);
    if (layer_configured)
        draw_panel();
}
static const struct seat_listener seat_events = { on_capabilities, on_seat_name, on_input_method };

static void on_global(void *user, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    if (strcmp(iface, "compositor") == 0) compositor = registry_bind(registry, name, iface, version, &compositor_interface, 1);
    else if (strcmp(iface, "shm") == 0) shm = registry_bind(registry, name, iface, version, &shm_interface, 1);
    else if (strcmp(iface, "shell") == 0) shell = registry_bind(registry, name, iface, version, &shell_interface, 1);
    else if (strcmp(iface, "seat") == 0) {
        seat = registry_bind(registry, name, iface, version, &seat_interface, 1);
        seat_add_listener(seat, &seat_events, NULL);
    }
    else if (strcmp(iface, "toplevel_manager") == 0) manager = registry_bind(registry, name, iface, version, &toplevel_manager_interface, 1);
    else if (strcmp(iface, "input_method_manager") == 0)
        imemenu_bind(registry_bind(registry, name, iface, version, &input_method_manager_interface, 1));
    else if (strcmp(iface, "output") == 0) {
        struct wire_proxy *o = registry_bind(registry, name, iface, version, &output_interface, 1);
        output_add_listener(o, &output_events, NULL);
    }
}
static void on_global_remove(void *user, struct wire_proxy *registry, uint32_t name) {}
static const struct registry_listener registry_events = { on_global, on_global_remove };

void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("panel: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

static void on_chld(int sig) {}

int main(void)
{
    setlocale(LC_ALL, "");
    textdomain("panel");
    signal(SIGCHLD, on_chld);
    signal(SIGPIPE, SIG_IGN);
    display = wire_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "panel: no X12 server\n");
        return 1;
    }
    struct wire_proxy *registry = display_get_registry(wire_display_proxy(display));
    registry_add_listener(registry, &registry_events, NULL);
    wire_display_roundtrip(display);
    wire_display_roundtrip(display);
    if (!compositor || !shm || !shell || !seat || !manager) {
        fprintf(stderr, "panel: missing globals\n");
        return 1;
    }
    pointer = seat_get_pointer(seat);
    pointer_add_listener(pointer, &pointer_events, NULL);
    toplevel_manager_add_listener(manager, &manager_events, NULL);
    launcher_init();
    theme_init_default(&ui);
    ui.metric[TM_FONT_PX] = 13;
    ui.metric[TM_RADIUS] = 5;
    theme_apply(&ui);
    if (canvas_create(&panel, screen_w, PANEL_H) < 0)
        return 1;
    layer = shell_get_layer_surface(shell, panel.surface, 2, "panel");
    layer_surface_add_listener(layer, &layer_events, NULL);
    layer_surface_set_anchor(layer, 2 | 4 | 8);
    layer_surface_set_exclusive_zone(layer, PANEL_H);
    /* A width of 0 with the anchors left and right takes the width of the
     * screen. The compositor then sends the new width after a mode change. */
    layer_surface_set_size(layer, 0, PANEL_H);
    while (!layer_configured)
        if (wire_display_dispatch(display) < 0)
            return 1;
    draw_panel();
    log_line("started");
    int tfd = timerfd_create(TFD_NONBLOCK | TFD_CLOEXEC);
    struct timerfd_spec spec = { 1000, 1000 };
    timerfd_settime(tfd, &spec);
    for (;;) {
        wire_display_flush(display);
        struct pollfd pf[3] = { { wire_display_fd(display), POLLIN, 0 }, { tfd, POLLIN, 0 }, { mixer_fd(), POLLIN, 0 } };
        if (poll(pf, pf[2].fd >= 0 ? 3 : 2, 1000) < 0)
            continue;
        if (pf[2].fd >= 0 && pf[2].revents)
            mixer_dispatch(pf[2].revents);
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        if (pf[1].revents & POLLIN) {
            uint64_t n;
            read(tfd, &n, 8);
            /* A language chosen in Settings applies to the panel and to
             * the programs that it starts from now on. */
            if (conf_export_locale()) {
                setlocale(LC_ALL, "");
                log_line("language %s", getenv("LANG"));
            }
            draw_panel();
        }
        if (pf[0].revents & (POLLIN | POLLHUP))
            if (wire_display_dispatch(display) < 0)
                break;
    }
    return 0;
}
