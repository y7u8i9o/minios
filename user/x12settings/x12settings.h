#pragma once
/* x12settings (docs/design/x12settings.md): the state and the settings of
 * the running X12. main.c connects, dispatches the events of the debug
 * and settings interfaces to the pages and builds the window. Each page
 * has its own file. */
#include <stdint.h>
#include <gui/app.h>
#include <wire/client.h>

extern struct app *app;
extern struct wire_proxy *debug_proxy;     /* debug version 3 */

/* Sends a setting to X12, unless a page applies a value from X12. */
void setting_send(const char *key, int value);

/* perf.c: the Performance page. perf_tick sends its requests once per
 * second. */
void perf_build(struct widget *tabs);
void perf_tick(void);
void perf_stats(uint32_t uptime_ms, uint32_t clients, uint32_t surfaces, uint32_t frame_ms);
void perf_frame_stat(const char *key, uint64_t value);
void perf_frame_stats_done(void);
void perf_frame(uint32_t serial, uint32_t total_us, uint32_t pixels, uint32_t latency_us);
void perf_history_done(uint32_t last);

/* clients.c: the surfaces of X12. */
void clients_build(struct widget *tabs);
void clients_tick(void);
void clients_surface(uint32_t id, uint32_t client, const char *role, const char *title, int32_t x, int32_t y,
                     int32_t w, int32_t h, uint32_t mapped, uint32_t format);
void clients_surfaces_done(void);
void clients_client(uint32_t number, uint32_t pid, uint32_t uid, uint32_t surfaces, uint32_t pool_bytes,
                    uint32_t not_responding);
void clients_done(void);
void clients_surface_info(uint32_t id, uint32_t found, int32_t bw, int32_t bh, int32_t scale, int32_t transform,
                          uint32_t format, uint32_t opaque, uint32_t commits);
void clients_captured(uint32_t id, int32_t w, int32_t h);
void clients_capture_failed(uint32_t id);
/* "clients", "capture TITLE" and "highlight TITLE [SECONDS]" without a
 * window. Returns the exit status. */
int clients_command(int argc, char **argv);

/* live.c: the settings page. live_value applies a value that X12 reports. */
void live_build(struct widget *tabs);
void live_tick(void);
void live_value(const char *key, int32_t value);
void live_method(uint32_t index, const char *name, const char *title, uint32_t current);
void live_methods_done(void);

/* inspect.c: the Debug page with the debug views and the pixel reader. */
void inspect_build(struct widget *tabs);
void inspect_value(const char *key, int32_t value);
void inspect_pixel(int32_t x, int32_t y, uint32_t value);
