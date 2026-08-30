/* panel: a layer surface at the bottom of the screen with a launcher
 * menu (a popup), one button per toplevel from the toplevel manager,
 * and a clock. Built directly on libwire and gfx. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/timerfd.h>
#include <wire/client.h>
#include <gui/gfx.h>
#include "core-client.h"
#include "shell-client.h"
#include "seat-client.h"

#define PANEL_H 28
#define MENU_BTN_W 64
#define TASK_BTN_W 120
#define CLOCK_W 80
#define MENU_ITEM_H 20
#define MENU_W 176
#define MAX_ENTRIES 16
#define MAX_TASKS 16

struct entry { char title[24]; char path[64]; };
struct task { struct wire_proxy *handle; char title[48]; int active, minimized; };

/* A drawable surface backed by one shm buffer. */
struct canvas {
    struct wire_proxy *surface, *buffer, *pool;
    struct surface s;
    int fd;
};

static struct wire_display *display;
static struct wire_proxy *compositor, *shm, *shell, *seat, *pointer, *manager;
static struct canvas panel, menu;
static struct wire_proxy *layer, *popup;
static struct entry entries[MAX_ENTRIES];
static int nentries;
static struct task tasks[MAX_TASKS];
static int ntasks;
static int screen_w = 1024, screen_h = 768;
static int menu_open, hover_item = -1;
static int px, py;                       /* pointer in the panel */
static struct wire_proxy *pointer_surface;
static uint32_t last_serial;
static int layer_configured;

static void log_line(const char *fmt, ...);

/* ---- canvases ---- */

static int canvas_create(struct canvas *c, int w, int h)
{
    size_t size = (size_t)w * h * 4;
    c->fd = memfd_create("panel", MFD_CLOEXEC);
    if (c->fd < 0 || ftruncate(c->fd, (long)size) < 0)
        return -1;
    c->s.pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
    if (c->s.pixels == MAP_FAILED)
        return -1;
    c->s.width = w;
    c->s.height = h;
    c->s.stride = w;
    c->pool = shm_create_pool(shm, c->fd, (int32_t)size);
    c->buffer = shm_pool_create_buffer(c->pool, 0, w, h, w * 4, 1);
    c->surface = compositor_create_surface(compositor);
    return 0;
}

static void canvas_commit(struct canvas *c)
{
    surface_attach(c->surface, c->buffer, 0, 0);
    surface_damage(c->surface, 0, 0, c->s.width, c->s.height);
    surface_commit(c->surface);
}

/* ---- drawing ---- */

static void draw_panel(void)
{
    struct surface *s = &panel.s;
    gfx_fill(s, 0x00202830);
    gfx_hline(s, 0, 0, s->width, 0x00101010);
    gfx_fill_rect(s, 4, 4, MENU_BTN_W, PANEL_H - 8, menu_open ? 0x00405870 : 0x00303c48);
    gfx_text(s, 20, 6, "Menu", 0x00e0e0e0, 0xffffffffu);
    int limit = (screen_w - CLOCK_W - MENU_BTN_W - 16) / (TASK_BTN_W + 4);
    for (int i = 0; i < ntasks && i < limit; i++) {
        int x = MENU_BTN_W + 12 + i * (TASK_BTN_W + 4);
        gfx_fill_rect(s, x, 4, TASK_BTN_W, PANEL_H - 8, tasks[i].active && !tasks[i].minimized ? 0x00405870 : 0x00303c48);
        char label[16];
        snprintf(label, sizeof label, "%s%.13s", tasks[i].minimized ? "_" : "", tasks[i].title);
        gfx_text(s, x + 4, 6, label, 0x00e0e0e0, 0xffffffffu);
    }
    long sec = uptime_ms() / 1000;
    char t[16];
    snprintf(t, sizeof t, "%02ld:%02ld:%02ld", sec / 3600, (sec / 60) % 60, sec % 60);
    gfx_text(s, s->width - CLOCK_W + 8, 6, t, 0x00e0e0e0, 0xffffffffu);
    canvas_commit(&panel);
}

static void draw_menu(void)
{
    struct surface *s = &menu.s;
    gfx_fill(s, 0x00f0f0f0);
    gfx_rect(s, 0, 0, s->width, s->height, 0x00101010);
    for (int i = 0; i < nentries; i++) {
        int y = 2 + i * MENU_ITEM_H;
        if (i == hover_item)
            gfx_fill_rect(s, 1, y, s->width - 2, MENU_ITEM_H, 0x00c0d0e0);
        gfx_text(s, 8, y + 2, entries[i].title, 0x00000000, 0xffffffffu);
    }
    canvas_commit(&menu);
}

/* ---- launcher ---- */

static void load_entries(void)
{
    FILE *f = fopen("/etc/launcher", "r");
    if (!f)
        return;
    char line[128];
    while (nentries < MAX_ENTRIES && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (!eq || line[0] == '#')
            continue;
        *eq = '\0';
        strlcpy(entries[nentries].title, line, sizeof entries[0].title);
        strlcpy(entries[nentries].path, eq + 1, sizeof entries[0].path);
        nentries++;
    }
    fclose(f);
}

static void launch(const struct entry *e)
{
    if (strcmp(e->path, "@logout") == 0) {
        log_line("logout");
        exit(0);                        /* startgui ends the session when the panel exits */
    }
    log_line("launch %s", e->path);
    pid_t pid = fork();
    if (pid == 0) {
        char *const args[] = { (char *)e->path, NULL };
        execvp(e->path, args);
        _exit(127);
    }
}

/* ---- popup events ---- */

static void on_popup_configure(void *user, struct wire_proxy *p, uint32_t serial, int32_t x, int32_t y, int32_t w, int32_t h)
{
    popup_ack_configure(p, serial);
}
static void menu_teardown(void);
static void on_popup_done(void *user, struct wire_proxy *p)
{
    /* The compositor dismissed the menu: release the popup and its
     * surface, or the next menu_show would ask for a role on a surface
     * that still has one, which is a protocol error. */
    menu_open = 0;
    hover_item = -1;
    menu_teardown();
    log_line("menu closed");
    draw_panel();
}
static const struct popup_listener popup_events = { on_popup_configure, on_popup_done };

static void menu_show(void)
{
    int h = nentries * MENU_ITEM_H + 4;
    if (!menu.surface && canvas_create(&menu, MENU_W, h) < 0)
        return;
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, MENU_W, h);
    positioner_set_anchor_rect(pos, 4, 4, MENU_BTN_W, 1);
    positioner_set_anchor(pos, 5);             /* top left of the button */
    positioner_set_gravity(pos, 7);            /* extends up and to the right */
    popup = shell_get_popup(shell, menu.surface, panel.surface, pos);
    popup_add_listener(popup, &popup_events, NULL);
    positioner_destroy(pos);
    popup_grab(popup, seat, last_serial);
    menu_open = 1;
    draw_menu();
    log_line("menu opened");
    draw_panel();
}

static void menu_hide(void)
{
    if (!menu_open)
        return;
    menu_open = 0;
    hover_item = -1;
    menu_teardown();
    draw_panel();
}

static void menu_teardown(void)
{
    if (!menu.surface)
        return;
    if (popup) {
        popup_destroy(popup);
        popup = NULL;
    }
    surface_attach(menu.surface, NULL, 0, 0);
    surface_commit(menu.surface);
    surface_destroy(menu.surface);
    buffer_destroy(menu.buffer);
    shm_pool_destroy(menu.pool);
    munmap(menu.s.pixels, (size_t)menu.s.width * menu.s.height * 4);
    close(menu.fd);
    memset(&menu, 0, sizeof menu);
    draw_panel();
}

/* ---- pointer ---- */

static void on_enter(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *surface, int32_t x, int32_t y)
{
    pointer_surface = surface;
    px = wire_fixed_to_int(x);
    py = wire_fixed_to_int(y);
    last_serial = serial;
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
    if (menu_open && pointer_surface == menu.surface) {
        int item = (py - 2) / MENU_ITEM_H;
        if (item >= nentries) item = -1;
        if (item != hover_item) {
            hover_item = item;
            draw_menu();
        }
    }
}
static void on_button(void *user, struct wire_proxy *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    last_serial = serial;
    if (button != 1 || state != 1)
        return;
    if (pointer_surface == menu.surface) {
        int item = (py - 2) / MENU_ITEM_H;
        menu_hide();
        if (item >= 0 && item < nentries)
            launch(&entries[item]);
        return;
    }
    if (pointer_surface != panel.surface)
        return;
    if (px >= 4 && px < 4 + MENU_BTN_W) {
        if (menu_open)
            menu_hide();
        else
            menu_show();
        return;
    }
    int limit = (screen_w - CLOCK_W - MENU_BTN_W - 16) / (TASK_BTN_W + 4);
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
    layer_configured = 1;
}
static void on_layer_closed(void *user, struct wire_proxy *l) { exit(0); }
static const struct layer_surface_listener layer_events = { on_layer_configure, on_layer_closed };

static void on_geometry(void *user, struct wire_proxy *o, int32_t x, int32_t y, int32_t w, int32_t h) { screen_w = w; screen_h = h; }
static void on_mode(void *user, struct wire_proxy *o, int32_t w, int32_t h, int32_t r) {}
static const struct output_listener output_events = { on_geometry, on_mode };

static void on_global(void *user, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    if (strcmp(iface, "compositor") == 0) compositor = registry_bind(registry, name, iface, version, &compositor_interface, 1);
    else if (strcmp(iface, "shm") == 0) shm = registry_bind(registry, name, iface, version, &shm_interface, 1);
    else if (strcmp(iface, "shell") == 0) shell = registry_bind(registry, name, iface, version, &shell_interface, 1);
    else if (strcmp(iface, "seat") == 0) seat = registry_bind(registry, name, iface, version, &seat_interface, 1);
    else if (strcmp(iface, "toplevel_manager") == 0) manager = registry_bind(registry, name, iface, version, &toplevel_manager_interface, 1);
    else if (strcmp(iface, "output") == 0) {
        struct wire_proxy *o = registry_bind(registry, name, iface, version, &output_interface, 1);
        output_add_listener(o, &output_events, NULL);
    }
}
static void on_global_remove(void *user, struct wire_proxy *registry, uint32_t name) {}
static const struct registry_listener registry_events = { on_global, on_global_remove };

static void log_line(const char *fmt, ...)
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
    signal(SIGCHLD, on_chld);
    signal(SIGPIPE, SIG_IGN);
    display = wire_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "panel: no compositor\n");
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
    load_entries();
    if (canvas_create(&panel, screen_w, PANEL_H) < 0)
        return 1;
    layer = shell_get_layer_surface(shell, panel.surface, 2, "panel");
    layer_surface_add_listener(layer, &layer_events, NULL);
    layer_surface_set_anchor(layer, 2 | 4 | 8);
    layer_surface_set_exclusive_zone(layer, PANEL_H);
    layer_surface_set_size(layer, screen_w, PANEL_H);
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
        struct pollfd pf[2] = { { wire_display_fd(display), POLLIN, 0 }, { tfd, POLLIN, 0 } };
        if (poll(pf, 2, 1000) < 0)
            continue;
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        if (pf[1].revents & POLLIN) {
            uint64_t n;
            read(tfd, &n, 8);
            draw_panel();
        }
        if (pf[0].revents & (POLLIN | POLLHUP))
            if (wire_display_dispatch(display) < 0)
                break;
    }
    return 0;
}
