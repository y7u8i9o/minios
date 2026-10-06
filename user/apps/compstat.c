/* compstat: the frame statistics of X12 (docs/design/graphics-performance.md).
 * Without arguments it prints every value on one line as key=value pairs,
 * with -r it resets the statistics, and with -r and -p it prints and then
 * resets them. -h prints the recorded frames of X12, one per line, and a
 * line with their number (debug version 3, docs/design/x12settings.md). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gui/client.h>
#include <wire/client.h>
#include "debug-client.h"

static char line[2048];
static size_t used;

static void on_frame_stat(void *user, struct wire_proxy *p, const char *key, uint32_t high, uint32_t low)
{
    unsigned long long v = (unsigned long long)high << 32 | low;
    int n = snprintf(line + used, sizeof line - used, " %s=%llu", key, v);
    if (n > 0 && (size_t)n < sizeof line - used)
        used += (size_t)n;
}

static void on_frame_stats_done(void *user, struct wire_proxy *p)
{
    printf("compstat:%s\n", line);
    fflush(stdout);
    used = 0;
    line[0] = 0;
}

static int history_frames;

static void on_frame(void *user, struct wire_proxy *p, uint32_t serial, uint32_t end_ms, uint32_t total_us,
                     uint32_t compose_us, uint32_t flush_us, uint32_t pixels, uint32_t latency_us)
{
    printf("compstat: frame %u end_ms=%u total_us=%u compose_us=%u flush_us=%u pixels=%u latency_us=%u\n", serial,
           end_ms, total_us, compose_us, flush_us, pixels, latency_us);
    history_frames++;
}

static void on_frame_history_done(void *user, struct wire_proxy *p, uint32_t last)
{
    printf("compstat: history of %d frames, the newest %u\n", history_frames, last);
    fflush(stdout);
}

static const struct debug_listener events = { .frame_stat = on_frame_stat, .frame_stats_done = on_frame_stats_done,
                                              .frame = on_frame, .frame_history_done = on_frame_history_done };

static void usage(void)
{
    fprintf(stderr, "usage: compstat [-r] [-p] | -h\n");
}

int main(int argc, char **argv)
{
    int reset = 0, print = 0, history = 0, opt;
    while ((opt = getopt(argc, argv, "rph")) != -1) {
        if (opt == 'r') {
            reset = 1;
        } else if (opt == 'p') {
            print = 1;
        } else if (opt == 'h') {
            history = 1;
        } else {
            usage();
            return 2;
        }
    }
    if (optind != argc) {
        usage();
        return 2;
    }
    if (history && (reset || print)) {
        usage();
        return 2;
    }
    if (!reset && !history)
        print = 1;
    if (gui_connect() < 0) {
        perror("compstat: cannot connect to X12");
        return 1;
    }
    int version = history ? 3 : 2;
    struct wire_proxy *debug = gui_bind_global("debug", &debug_interface, version);
    if (!debug) {
        fprintf(stderr, "compstat: X12 has no debug interface of version %d\n", version);
        gui_disconnect();
        return 1;
    }
    debug_add_listener(debug, &events, NULL);
    if (print)
        debug_get_frame_stats(debug);
    if (reset)
        debug_reset_frame_stats(debug);
    if (history)
        debug_get_frame_history(debug, 0);
    int r = wire_display_roundtrip(gui_display());
    if (reset && !print) {
        printf("compstat: reset\n");
        fflush(stdout);
    }
    gui_disconnect();
    return r < 0 ? 1 : 0;
}
