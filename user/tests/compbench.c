/* compbench: a framework window of 760x540 that runs one benchmark
 * scenario of the frame statistics (docs/design/graphics-performance.md)
 * and prints the statistics of X12 and of the client for it.
 *
 * compbench SCENARIO
 *   blink    a canvas of 2x20 pixels changes colour 20 times, every 100 ms
 *   anim     the whole window is painted again 60 times, every 16 ms
 *   idle     nothing changes for two seconds
 *   window   the window alone, until it is closed, for the scenarios
 *            that the boot test drives and measures with compstat. The
 *            client line follows the close request.
 *
 * The widget scenarios of K2 of docs/plan/widgets.md send their input to
 * their own window with window_message, one step every 100 ms. The counts
 * therefore do not depend on the timing of the pointer.
 *   wheel    a table of 60 rows with 20 visible: 10 wheel steps up at the
 *            top, then 20 down. 14 of the 20 change the view.
 *   type     20 characters into a text field of 300 pixels
 *   status   the label of a status bar set 20 times
 *   edit     20 characters at line 3500 of an editor with 4000 lines of C
 *            and the highlighter
 *   scroll   20 wheel steps in a scroll area with 200 labels
 *   hover    the pointer moved across 20 rows of the table
 *
 * The window settles for 800 ms, then compbench resets the statistics of
 * X12, runs the scenario, waits 300 ms for the last frame and prints two
 * lines: "compbench: SCENARIO x12 key=value ..." with every value of
 * compstat, and "compbench: SCENARIO client key=value ..." with the
 * values of gui_get_stats during the scenario. */
#include <gui/model.h>
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

static void print_client_stats(void)
{
    struct gui_stats now;
    gui_get_stats(&now);
    printf("compbench: %s client paints=%llu paint_us=%llu commits=%llu copy_us=%llu copied_bytes=%llu "
           "frame_waits=%llu pool_bytes=%llu widget_paints=%llu painted_pixels=%llu layouts=%llu text_shapes=%llu\n",
           scenario, (unsigned long long)(now.paints - client_at_start.paints),
           (unsigned long long)(now.paint_us - client_at_start.paint_us),
           (unsigned long long)(now.commits - client_at_start.commits),
           (unsigned long long)(now.copy_us - client_at_start.copy_us),
           (unsigned long long)(now.copied_bytes - client_at_start.copied_bytes),
           (unsigned long long)(now.frame_waits - client_at_start.frame_waits), (unsigned long long)now.pool_bytes,
           (unsigned long long)(now.widget_paints - client_at_start.widget_paints),
           (unsigned long long)(now.painted_pixels - client_at_start.painted_pixels),
           (unsigned long long)(now.layouts - client_at_start.layouts),
           (unsigned long long)(now.text_shapes - client_at_start.text_shapes));
    fflush(stdout);
}

static void on_frame_stats_done(void *user, struct wire_proxy *p)
{
    printf("compbench: %s x12%s\n", scenario, line);
    print_client_stats();
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

/* The window scenario prints the client statistics before the window
 * closes. The case gui_memory reads the pool bytes from them. */
static int on_close(struct widget *w, void *args, void *arg)
{
    print_client_stats();
    return 0;
}

/* ---- the widget scenarios ---- */

#define TABLE_ROWS 60
#define TABLE_VISIBLE 20
#define EDIT_LINES 4000
#define EDIT_LINE 3500

static struct widget *table, *field, *status_label, *editor, *area;

static int model_rows(struct model *m, int parent) { return parent < 0 ? TABLE_ROWS : 0; }
static int model_child(struct model *m, int parent, int index) { return index; }
static int model_columns(struct model *m) { return 2; }

static const char *model_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    snprintf(buf, size, col ? "%d" : "Row %d", col ? row * 37 % 101 : row);
    return buf;
}

static const char *model_header(struct model *m, int col) { return col ? "Value" : "Name"; }

static struct model rows_model = { model_rows, model_child, model_columns, model_cell, model_header };

/* The row height of the table: the font height plus 6 (models.c). */
static int table_row_h(void)
{
    return app_theme(app)->font->height + 6;
}

static void send_mouse(struct widget *w, int kind, int x, int y, int buttons)
{
    int ax, ay;
    widget_abs(w, &ax, &ay);
    struct wmsg m = { .type = WM_MOUSE, .window = window_state_of(window)->win->id, .a = ax + x, .b = ay + y,
                      .c = buttons, .d = kind };
    window_message(window, &m);
}

static void send_char(int ch)
{
    struct wmsg m = { .type = WM_KEY, .window = window_state_of(window)->win->id, .a = 0, .b = 1, .d = ch };
    window_message(window, &m);
}

/* 4000 lines of C with keywords, strings, numbers and comments. */
static char *c_source(void)
{
    size_t size = (size_t)EDIT_LINES * 64 + 1, used_bytes = 0;
    char *text = malloc(size);
    if (!text)
        return NULL;
    for (int i = 0; i < EDIT_LINES && used_bytes < size; i++) {
        const char *forms[] = { "static int value_%d = %d; /* line */\n", "    if (x > %d) return %d;\n",
                                "    printf(\"%%d\\n\", %d + %d);\n", "#define LIMIT_%d %d\n" };
        int n = snprintf(text + used_bytes, size - used_bytes, forms[i % 4], i, i * 7);
        if (n < 0)
            break;
        used_bytes += (size_t)n;
    }
    return text;
}

static void build_widget_scenario(struct widget *box)
{
    if (strcmp(scenario, "wheel") == 0 || strcmp(scenario, "hover") == 0) {
        table = table_new(box);
        view_set_model(table, &rows_model);
        widget_set_stretch(table, 1, 0);
        widget_set_hint(table, 0, 2 + 24 + TABLE_VISIBLE * table_row_h());
    } else if (strcmp(scenario, "type") == 0) {
        field = textfield_new(box, "");
        widget_set_hint(field, 300, 0);
        widget_set_stretch(field, 0, 0);
        widget_focus(field);
    } else if (strcmp(scenario, "status") == 0) {
        struct widget *body = canvas_new(box);
        widget_connect(body, "paint", paint_scene, NULL);
        struct widget *bar = statusbar_new(box);
        status_label = statusbar_add(bar, 1);
        widget_set_text(status_label, "Ready");
        widget_set_text(statusbar_add(bar, 0), "Line 1");
    } else if (strcmp(scenario, "edit") == 0) {
        editor = editor_new(box);
        editor_set_highlighter(editor, highlight_c, NULL);
        editor_set_line_numbers(editor, 1);
        char *text = c_source();
        editor_set_text(editor, text ? text : "");
        free(text);
        editor_goto(editor, EDIT_LINE - 1, 0);
        widget_focus(editor);
    } else if (strcmp(scenario, "scroll") == 0) {
        area = scrollarea_new(box);
        for (int i = 0; i < 200; i++) {
            char text[32];
            snprintf(text, sizeof text, "Label %d", i);
            label_new(area->user, text);
        }
    }
}

static void widget_step(int i)
{
    if (strcmp(scenario, "wheel") == 0) {
        send_mouse(table, WMOUSE_WHEEL, 40, 60, i < 10 ? -1 : 1);
    } else if (strcmp(scenario, "hover") == 0) {
        send_mouse(table, WMOUSE_MOVE, 40, 1 + 24 + i * table_row_h() + table_row_h() / 2, 0);
    } else if (strcmp(scenario, "type") == 0 || strcmp(scenario, "edit") == 0) {
        send_char('a' + i % 26);
    } else if (strcmp(scenario, "status") == 0) {
        char text[32];
        snprintf(text, sizeof text, "Step %d", i + 1);
        widget_set_text(status_label, text);
    } else if (strcmp(scenario, "scroll") == 0) {
        send_mouse(area, WMOUSE_WHEEL, 40, 40, 1);
    }
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
    } else {
        widget_step(done_steps);
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
        fprintf(stderr, "usage: compbench blink|anim|idle|window|wheel|type|status|edit|scroll|hover\n");
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
    } else if (strcmp(scenario, "wheel") == 0) {
        steps = 30;
        step_ms = 100;
    } else if (strcmp(scenario, "type") == 0 || strcmp(scenario, "status") == 0 || strcmp(scenario, "edit") == 0 ||
               strcmp(scenario, "scroll") == 0 || strcmp(scenario, "hover") == 0) {
        steps = 20;
        step_ms = 100;
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
    if (strcmp(scenario, "blink") && strcmp(scenario, "anim") && strcmp(scenario, "idle") &&
        strcmp(scenario, "window")) {
        build_widget_scenario(box);
        app_timer_add(app, SETTLE_MS, 0, start, NULL);
        int code = app_run(app);
        app_destroy(app);
        return code;
    }
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
    else
        widget_connect(window, "close", on_close, NULL);
    int code = app_run(app);
    app_destroy(app);
    return code;
}
