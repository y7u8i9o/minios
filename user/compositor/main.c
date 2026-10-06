/* X12: a listening socket, clients, the input devices (input.c) and the
 * frame clock in one poll loop. Every notable event is logged as
 * "x12: ..." for the tests.
 *
 * The frame clock runs only while work is pending (G6 of
 * docs/plan/compositor-performance.md). After each pass of the loop X12
 * composes at once when damage or a frame callback is pending and
 * frame_ms have passed since the last frame. Otherwise it arms a one-shot
 * timer for the rest of the interval. Without pending work the timer is
 * disarmed, and poll waits until the earliest deadline of the key
 * repeat, the pings and the input method, or for input and clients. */
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/timerfd.h>
#include <minios/abi.h>
#include "comp.h"

void client_attach(struct wire_client *wc);

static struct wire_server *srv;
static volatile int running = 1;
static uint32_t serial = 1;
static int frame_fd;
static int frame_armed;                 /* the one-shot timer runs */
static long last_frame_us;              /* the start of the last frame */
int cursor_x, cursor_y;

/* The log goes to /var/log/x12.log (truncated at start); -s mirrors it
 * to standard output, which the boot tests read on the serial line, and
 * the file's absence (a read-only root) falls back to standard output.
 * comp_debug lines (per frame, per key, per commit) need the verbose
 * setting (-v or the debug interface). */
static FILE *logfile;
static int log_serial;

static void vlog(const char *fmt, va_list ap)
{
    if (logfile) {
        va_list copy;
        va_copy(copy, ap);
        fprintf(logfile, "x12: ");
        vfprintf(logfile, fmt, copy);
        fputc('\n', logfile);
        fflush(logfile);
        va_end(copy);
    }
    if (log_serial || !logfile) {
        printf("x12: ");
        vprintf(fmt, ap);
        printf("\n");
        fflush(stdout);
    }
}

void comp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void comp_debug(const char *fmt, ...)
{
    if (!settings.verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

static void log_open(void)
{
    mkdir("/var", 0755);
    mkdir("/var/log", 0755);
    logfile = fopen("/var/log/x12.log", "w");
}

uint32_t comp_serial(void) { return serial++; }

static void on_term(int sig) { running = 0; }

static long frames_since_report, report_at;

static void arm_frame_timer(long ms)
{
    struct timerfd_spec spec = { (uint32_t)ms, 0 };
    timerfd_settime(frame_fd, &spec);
    frame_armed = ms > 0;
}

/* The setting frame_ms changed: the next frame follows the new minimum
 * interval. */
void frame_clock_set(int ms)
{
    (void)ms;
    if (frame_armed)
        arm_frame_timer(0);
}

/* Flush every client and note since when a socket has remained full; a
 * client that stops reading or answering pings is shown as not
 * responding (hang.c) and dropped only through its Force quit button. */
static void flush_clients(void)
{
    long now = uptime_ms();
    for (struct wire_client *c = wire_server_first_client(srv); c; c = wire_client_next(c)) {
        wire_client_flush(c);
        struct client *cl = wire_client_get_user_data(c);
        if (cl) {
            if (wire_client_pending(c) == 0)
                cl->stall_since = 0;
            else if (!cl->stall_since)
                cl->stall_since = now;
        }
    }
    hang_tick(now);
    im_tick(now);
    seat_tick(now);
}

static void frame(void)
{
    last_frame_us = uptime_us();
    int presented = scene_has_damage();
    if (presented) {
        scene_compose();
        frames_since_report++;
        comp_debug("frame");
    } else {
        stats_idle_timer();
    }
    /* After a composition the callbacks follow the flush of the frame. A
     * commit that carried only a frame callback adds no damage, and its
     * callback completes in a frame without a composition, so that the
     * client may draw its next frame. */
    surfaces_frame_done((uint32_t)uptime_ms());
    flush_clients();
    /* One line every ten seconds while frames are composed, instead of
     * a line per frame. */
    long now = uptime_ms();
    if (now >= report_at) {
        if (frames_since_report)
            comp_debug("%ld frames in the last %ld s", frames_since_report, (now - (report_at - 10000)) / 1000);
        frames_since_report = 0;
        report_at = now + 10000;
    }
}

/* Compose now when damage or a frame callback is pending and the minimum
 * interval since the last frame has passed. Otherwise arm the one-shot
 * timer for the rest of the interval, or disarm it when nothing is
 * pending. */
static void schedule_frame(void)
{
    if (!scene_has_damage() && !surfaces_frame_pending()) {
        if (frame_armed)
            arm_frame_timer(0);
        return;
    }
    long now = uptime_us(), due = last_frame_us + (long)settings.frame_ms * 1000;
    if (now >= due) {
        if (frame_armed)
            arm_frame_timer(0);
        frame();
        return;
    }
    if (!frame_armed)
        arm_frame_timer((due - now + 999) / 1000);
}

/* The poll timeout until the earliest deadline of the modules, -1 for
 * none. */
static int poll_timeout(void)
{
    long deadlines[3] = { seat_next_deadline(), hang_next_deadline(), im_next_deadline() }, next = -1;
    for (int i = 0; i < 3; i++)
        if (deadlines[i] >= 0 && (next < 0 || deadlines[i] < next))
            next = deadlines[i];
    if (next < 0)
        return -1;
    long wait = next - uptime_ms();
    return wait <= 0 ? 0 : wait > 60000 ? 60000 : (int)wait;
}

/* The scale of the last mode that a client or the boot chose. The backend
 * reduces the scale of a small mode to 1. A larger mode that follows the
 * host display then returns to the chosen scale. */
static int chosen_scale;

int comp_set_mode(int width, int height, int scale)
{
    if (width < 640 || height < 480 || scale < 1 || scale > 4)
        return -1;
    if (backend_set_mode(width, height, scale) < 0)
        return -1;
    chosen_scale = scale;
    settings.display_mode = DISPLAY_MODE_PACK(screen_w * screen_scale, screen_h * screen_scale, screen_scale);
    input_place_cursor(cursor_x >= screen_w ? screen_w - 1 : cursor_x, cursor_y >= screen_h ? screen_h - 1 : cursor_y);
    scene_set_cursor(cursor_x, cursor_y);
    shell_output_changed();
    output_changed();
    debug_screen_changed();
    scene_damage_all();
    comp_log("mode %dx%d scale %d", screen_w, screen_h, screen_scale);
    return 0;
}

void comp_follow_display(void)
{
    int width, height;
    if (backend_display_request(&width, &height) < 0 || !settings.display_follow)
        return;
    int scale = chosen_scale ? chosen_scale : screen_scale;
    if (width == screen_w * screen_scale && height == screen_h * screen_scale && scale == screen_scale)
        return;
    comp_log("the host display requests %dx%d", width, height);
    if (comp_set_mode(width, height, scale) == 0) {
        /* Every settings client sees the new mode. */
        debug_setting_changed("display_mode", settings.display_mode);
        return;
    }
    comp_log("mode %dx%d scale %d refused", width, height, scale);
}

/* x12 [-s] [-v] [socket name] */
int main(int argc, char **argv)
{
    const char *name = "display";
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0)
            log_serial = 1;
        else if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else
            name = argv[i];
    }
    log_open();
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);           /* a dead client must not kill the compositor */
    if (backend_init() < 0) {
        perror("x12: framebuffer");
        return 1;
    }
    if (input_init() < 0) {
        fprintf(stderr, "x12: no keyboard and pointer under /dev/input\n");
        return 1;
    }
    srv = wire_server_create(name);
    if (!srv) {
        perror("x12: listen");
        return 1;
    }
    surfaces_init(srv);
    shell_init(srv);
    hang_init(srv);
    seat_init(srv);
    data_init(srv);
    text_init(srv);
    im_init(srv);
    debug_init(srv);
    trace_init(srv);
    if (verbose)
        settings.verbose = 1;
    stats_reset();
    scene_init();
    decor_init();
    input_place_cursor(screen_w / 2, screen_h / 2);
    frame_fd = timerfd_create(TFD_NONBLOCK | TFD_CLOEXEC);
    scene_damage_all();
    report_at = uptime_ms() + 10000;
    settings.display_mode = DISPLAY_MODE_PACK(screen_w * screen_scale, screen_h * screen_scale, screen_scale);
    if (screen_scale > 1)
        comp_log("started %dx%d scale %d", screen_w, screen_h, screen_scale);
    else
        comp_log("started %dx%d", screen_w, screen_h);
    while (running) {
        /* The work of the previous pass, and at the start the first
         * frame, before poll waits. */
        schedule_frame();
        struct pollfd pf[OPEN_MAX];
        int n = 0;
        pf[n++] = (struct pollfd){ wire_server_fd(srv), POLLIN, 0 };
        pf[n++] = (struct pollfd){ frame_fd, POLLIN, 0 };
        int input_index = n;
        int ninput = input_fill_pollfds(pf + n, 16);
        n += ninput;
        int display_index = n;
        pf[n++] = (struct pollfd){ backend_display_fd(), POLLIN, 0 };
        int fetch_index = -1;
        if (data_fetch_fd() >= 0) {
            fetch_index = n;
            pf[n++] = (struct pollfd){ data_fetch_fd(), POLLIN, 0 };
        }
        int first_client = n;
        struct wire_client *clients[OPEN_MAX];
        int nclients = 0;
        for (struct wire_client *c = wire_server_first_client(srv); c && n < OPEN_MAX; c = wire_client_next(c)) {
            clients[nclients++] = c;
            pf[n++] = (struct pollfd){ wire_client_fd(c), POLLIN, 0 };
        }
        int r = poll(pf, (unsigned)n, poll_timeout());
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        stats_wakeup();
        if (pf[0].revents & POLLIN) {
            struct wire_client *c = wire_server_accept(srv);
            if (c)
                client_attach(c);
        }
        input_handle(pf + input_index, ninput);
        if (fetch_index >= 0 && (pf[fetch_index].revents & (POLLIN | POLLHUP)))
            data_fetch_read();
        if (pf[display_index].revents & POLLIN)
            comp_follow_display();
        for (int i = 0; i < nclients; i++)
            if (pf[first_client + i].revents & (POLLIN | POLLHUP)) {
                if (wire_client_dispatch(clients[i]) < 0)
                    wire_client_destroy(clients[i]);
            }
        flush_clients();
        if (pf[1].revents & POLLIN) {
            uint64_t expirations;
            read(frame_fd, &expirations, 8);
            frame_armed = 0;
        }
    }
    stats_log();
    wire_server_destroy(srv);
    input_close();
    backend_release();
    comp_log("stopped");
    if (logfile)
        fclose(logfile);
    return 0;
}
