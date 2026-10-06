/* compstat: the frame statistics of X12 (docs/design/graphics-performance.md).
 * Without arguments it prints every value on one line as key=value pairs,
 * with -r it resets the statistics, and with -r and -p it prints and then
 * resets them. */
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

static const struct debug_listener events = { .frame_stat = on_frame_stat, .frame_stats_done = on_frame_stats_done };

static void usage(void)
{
    fprintf(stderr, "usage: compstat [-r] [-p]\n");
}

int main(int argc, char **argv)
{
    int reset = 0, print = 0, opt;
    while ((opt = getopt(argc, argv, "rp")) != -1) {
        if (opt == 'r') {
            reset = 1;
        } else if (opt == 'p') {
            print = 1;
        } else {
            usage();
            return 2;
        }
    }
    if (optind != argc) {
        usage();
        return 2;
    }
    if (!reset)
        print = 1;
    if (gui_connect() < 0) {
        perror("compstat: cannot connect to X12");
        return 1;
    }
    struct wire_proxy *debug = gui_bind_global("debug", &debug_interface, 2);
    if (!debug) {
        fprintf(stderr, "compstat: X12 has no debug interface of version 2\n");
        gui_disconnect();
        return 1;
    }
    debug_add_listener(debug, &events, NULL);
    if (print)
        debug_get_frame_stats(debug);
    if (reset)
        debug_reset_frame_stats(debug);
    int r = wire_display_roundtrip(gui_display());
    if (reset && !print) {
        printf("compstat: reset\n");
        fflush(stdout);
    }
    gui_disconnect();
    return r < 0 ? 1 : 0;
}
