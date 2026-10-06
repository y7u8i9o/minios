/* compbench: a framework window of 760x540 that runs one benchmark
 * scenario of the frame statistics (docs/design/graphics-performance.md)
 * and prints the statistics of X12 and of the client for it.
 *
 * compbench SCENARIO
 *   blink    a canvas of 2x20 pixels changes colour 20 times, every 100 ms
 *   anim     the whole window is painted again 60 times, every 16 ms
 *   idle     nothing changes for two seconds
 *   window   the window alone, until it is closed, for the scenarios
 *            that the boot test drives and measures with compstat
 *
 * The window settles for 800 ms, then compbench resets the statistics of
 * X12, runs the scenario, waits 300 ms for the last frame and prints two
 * lines: "compbench: SCENARIO x12 key=value ..." with every value of
 * compstat, and "compbench: SCENARIO client key=value ..." with the
 * values of gui_get_stats during the scenario. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gui/app.h>
#include <gui/client.h>
#include "debug-client.h"

#define SETTLE_MS 800
#define FINISH_MS 300

static struct app *app;
static struct widget *window, *scene, *blinker;
static struct wire_proxy *debug;
static const char *scenario;
static int steps, step_ms, done_steps, blink_on;
static uint32_t scene_color = 0x00406080;
static struct gui_stats client_at_start;
static char line[2048];
static size_t used;

static void on_frame_stat(void *user, struct wire_proxy *p, const char *key, uint32_t high, uint32_t low)
{
    int n = snprintf(line + used, sizeof line - used, " %s=%llu", key, (unsigned long long)high << 32 | low);
    if (n > 0 && (size_t)n < sizeof line - used)
        used += (size_t)n;
}

static void on_frame_stats_done(void *user, struct wire_proxy *p)
{
    struct gui_stats now;
    gui_get_stats(&now);
    printf("compbench: %s x12%s\n", scenario, line);
    printf("compbench: %s client paints=%llu paint_us=%llu commits=%llu copy_us=%llu copied_bytes=%llu "
           "frame_waits=%llu pool_bytes=%llu\n",
           scenario, (unsigned long long)(now.paints - client_at_start.paints),
           (unsigned long long)(now.paint_us - client_at_start.paint_us),
           (unsigned long long)(now.commits - client_at_start.commits),
           (unsigned long long)(now.copy_us - client_at_start.copy_us),
           (unsigned long long)(now.copied_bytes - client_at_start.copied_bytes),
           (unsigned long long)(now.frame_waits - client_at_start.frame_waits), (unsigned long long)now.pool_bytes);
    fflush(stdout);
    app_quit(app, 0);
}

static const struct debug_listener debug_events = { .frame_stat = on_frame_stat,
                                                    .frame_stats_done = on_frame_stats_done };

static int paint_scene(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, scene_color);
    painter_fill(p, 20, 40, w->w / 2, w->h / 3, scene_color ^ 0x00ffffff);
    return 1;
}

static int paint_blinker(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, blink_on ? 0x00000000 : 0x00ffffff);
    return 1;
}

static void finish(void *arg)
{
    debug_get_frame_stats(debug);
    gui_flush();
}

static void step(void *arg)
{
    if (strcmp(scenario, "blink") == 0) {
        blink_on = !blink_on;
        widget_invalidate(blinker);
    } else if (strcmp(scenario, "anim") == 0) {
        scene_color = (scene_color + 0x00030507) & 0x00ffffff;
        widget_invalidate(scene);
    }
    if (++done_steps < steps)
        app_timer_add(app, step_ms, 0, step, NULL);
    else
        app_timer_add(app, FINISH_MS, 0, finish, NULL);
}

static void start(void *arg)
{
    debug_reset_frame_stats(debug);
    gui_flush();
    gui_get_stats(&client_at_start);
    if (steps)
        app_timer_add(app, step_ms, 0, step, NULL);
    else
        app_timer_add(app, step_ms + FINISH_MS, 0, finish, NULL);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: compbench blink|anim|idle|window\n");
        return 2;
    }
    scenario = argv[1];
    if (strcmp(scenario, "blink") == 0) {
        steps = 20;
        step_ms = 100;
    } else if (strcmp(scenario, "anim") == 0) {
        steps = 60;
        step_ms = 16;
    } else if (strcmp(scenario, "idle") == 0) {
        steps = 0;
        step_ms = 2000;
    } else if (strcmp(scenario, "window") == 0) {
        steps = -1;
    } else {
        fprintf(stderr, "compbench: unknown scenario %s\n", scenario);
        return 2;
    }
    app = app_create();
    if (!app)
        return 1;
    debug = gui_bind_global("debug", &debug_interface, 2);
    if (!debug) {
        fprintf(stderr, "compbench: X12 has no debug interface of version 2\n");
        return 1;
    }
    debug_add_listener(debug, &debug_events, NULL);
    window = app_window(app, 760, 540, "compbench");
    if (!window)
        return 1;
    widget_set_padding(window, 0);
    struct widget *box = box_new(window, 1);
    widget_set_stretch(box, 1, 1);
    blinker = canvas_new(box);
    widget_set_hint(blinker, 2, 20);
    widget_set_stretch(blinker, 0, 0);
    widget_set_align(blinker, ALIGN_START, ALIGN_START);
    widget_connect(blinker, "paint", paint_blinker, NULL);
    scene = canvas_new(box);
    widget_set_stretch(scene, 1, 1);
    widget_connect(scene, "paint", paint_scene, NULL);
    if (steps >= 0)
        app_timer_add(app, SETTLE_MS, 0, start, NULL);
    int code = app_run(app);
    app_destroy(app);
    return code;
}
