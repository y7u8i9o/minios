/* x12settings: the state and the settings of the running X12 through
 * the debug and settings interfaces (docs/design/x12settings.md).
 * "x12settings set KEY VALUE" applies one setting without a window.
 * "x12settings clients", "x12settings capture TITLE" and "x12settings
 * highlight TITLE [SECONDS]" print the clients and surfaces, capture a
 * surface and outline it on the screen. The
 * user's persistent choices belong to the Settings program. This tool
 * changes the running server until it exits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/client.h>
#include "x12settings.h"
#include "debug-client.h"

struct app *app;
struct wire_proxy *debug_proxy;
static struct wire_proxy *settings_proxy;

void setting_send(const char *key, int value)
{
    if (!settings_proxy)
        return;
    settings_set(settings_proxy, key, value);
    gui_flush();
}

/* ---- events ---- */

static void on_value(void *user, struct wire_proxy *p, const char *key, int32_t value)
{
    live_value(key, value);
    inspect_value(key, value);
}
static void on_settings_done(void *user, struct wire_proxy *p) {}
static const struct settings_listener settings_events = { on_value, on_settings_done };

static void on_stats(void *user, struct wire_proxy *p, uint32_t uptime, uint32_t comps, uint32_t ms, uint32_t max,
                     uint32_t clients, uint32_t surfaces, uint32_t frame_ms)
{
    perf_stats(uptime, clients, surfaces, frame_ms);
    printf("x12settings: up %u s, %u compositions (%u ms, max %u ms), %u clients, %u surfaces, frame %u ms\n",
           uptime / 1000, comps, ms, max, clients, surfaces, frame_ms);
    fflush(stdout);
}

static void on_surface(void *user, struct wire_proxy *p, uint32_t id, uint32_t client, const char *role,
                       const char *title, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t mapped, uint32_t format)
{
    clients_surface(id, client, role, title, x, y, w, h, mapped, format);
}

static void on_surfaces_done(void *user, struct wire_proxy *p) { clients_surfaces_done(); }
static void on_pixel(void *user, struct wire_proxy *p, int32_t x, int32_t y, uint32_t v) { inspect_pixel(x, y, v); }

static void on_frame_stat(void *user, struct wire_proxy *p, const char *key, uint32_t high, uint32_t low)
{
    perf_frame_stat(key, (uint64_t)high << 32 | low);
}

static void on_frame_stats_done(void *user, struct wire_proxy *p) { perf_frame_stats_done(); }

static void on_frame(void *user, struct wire_proxy *p, uint32_t serial, uint32_t end_ms, uint32_t total_us,
                     uint32_t compose_us, uint32_t flush_us, uint32_t pixels, uint32_t latency_us)
{
    perf_frame(serial, total_us, pixels, latency_us);
}

static void on_frame_history_done(void *user, struct wire_proxy *p, uint32_t last) { perf_history_done(last); }

static void on_client_info(void *user, struct wire_proxy *p, uint32_t number, uint32_t pid, uint32_t uid,
                           uint32_t surfaces, uint32_t pool_bytes, uint32_t not_responding)
{
    clients_client(number, pid, uid, surfaces, pool_bytes, not_responding);
}

static void on_clients_done(void *user, struct wire_proxy *p) { clients_done(); }

static void on_surface_info(void *user, struct wire_proxy *p, uint32_t id, uint32_t found, int32_t bw, int32_t bh,
                            int32_t scale, int32_t transform, uint32_t format, uint32_t opaque, uint32_t commits)
{
    clients_surface_info(id, found, bw, bh, scale, transform, format, opaque, commits);
}

static void on_surface_captured(void *user, struct wire_proxy *p, uint32_t id, int32_t w, int32_t h)
{
    clients_captured(id, w, h);
}

static void on_capture_failed(void *user, struct wire_proxy *p, uint32_t id) { clients_capture_failed(id); }

static void on_input_method_info(void *user, struct wire_proxy *p, uint32_t index, const char *name,
                                 const char *title, uint32_t current)
{
    live_method(index, name, title, current);
}

static void on_input_methods_done(void *user, struct wire_proxy *p) { live_methods_done(); }

static const struct debug_listener debug_events = {
    .stats = on_stats, .surface = on_surface, .surfaces_done = on_surfaces_done, .pixel = on_pixel,
    .frame_stat = on_frame_stat, .frame_stats_done = on_frame_stats_done, .frame = on_frame,
    .frame_history_done = on_frame_history_done, .client_info = on_client_info, .clients_done = on_clients_done,
    .surface_info = on_surface_info, .surface_captured = on_surface_captured, .capture_failed = on_capture_failed,
    .input_method_info = on_input_method_info, .input_methods_done = on_input_methods_done,
};

static void tick(void *arg)
{
    perf_tick();
    clients_tick();
    live_tick();
    gui_flush();
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    settings_proxy = gui_bind_global("settings", &settings_interface, 1);
    debug_proxy = gui_bind_global("debug", &debug_interface, 3);
    if (!settings_proxy || !debug_proxy) {
        fprintf(stderr, "x12settings: X12 has no settings interface or no debug interface of version 3\n");
        return 1;
    }
    settings_add_listener(settings_proxy, &settings_events, NULL);
    debug_add_listener(debug_proxy, &debug_events, NULL);
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        settings_set(settings_proxy, argv[2], atoi(argv[3]));
        gui_flush();
        struct wmsg ev;
        gui_next_event(&ev, 200);
        printf("x12settings: set %s %s\n", argv[2], argv[3]);
        fflush(stdout);
        app_destroy(app);
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "clients") == 0 || strcmp(argv[1], "capture") == 0 ||
                      strcmp(argv[1], "highlight") == 0)) {
        int status = clients_command(argc, argv);
        app_destroy(app);
        return status;
    }
    struct widget *win = app_window(app, 760, 560, "X12 settings");
    if (!win)
        return 1;
    struct widget *tabs = tabs_new(win);
    widget_set_stretch(tabs, 1, 1);
    perf_build(tabs);
    clients_build(tabs);
    live_build(tabs);
    inspect_build(tabs);
    settings_get_all(settings_proxy);
    tick(NULL);
    app_timer_add(app, 1000, 1, tick, NULL);
    app_run(app);
    app_destroy(app);
    return 0;
}
