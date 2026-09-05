/* X12: a listening socket, clients, the input devices (input.c) and a
 * 60 Hz frame clock in one poll loop. Every notable event is logged as
 * "x12: ..." for the tests. */
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
void surfaces_frame_done(uint32_t time_ms);
void scene_stats(void);

static struct wire_server *srv;
static volatile int running = 1;
static uint32_t serial = 1;
static int frame_fd;
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

void frame_clock_set(int ms)
{
    struct timerfd_spec spec = { ms, ms };
    timerfd_settime(frame_fd, &spec);
}

/* Flush every client and note since when a socket has stayed full; a
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
}

static void frame(void)
{
    uint64_t expirations;
    read(frame_fd, &expirations, 8);
    int presented = scene_has_damage();
    if (presented) {
        scene_compose();
        frames_since_report++;
        comp_debug("frame");
    }
    /* Completion means the back buffer has actually been copied to the
     * framebuffer.  Idle timer ticks are not presentations. */
    if (presented)
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

int comp_set_mode(int width, int height, int scale)
{
    if (width < 640 || height < 480 || scale < 1 || scale > 4)
        return -1;
    if (backend_set_mode(width, height, scale) < 0)
        return -1;
    settings.display_mode = DISPLAY_MODE_PACK(screen_w * screen_scale, screen_h * screen_scale, screen_scale);
    input_place_cursor(cursor_x >= screen_w ? screen_w - 1 : cursor_x, cursor_y >= screen_h ? screen_h - 1 : cursor_y);
    scene_set_cursor(cursor_x, cursor_y);
    shell_output_changed();
    output_changed();
    scene_damage_all();
    comp_log("mode %dx%d scale %d", screen_w, screen_h, screen_scale);
    return 0;
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
    debug_init(srv);
    if (verbose)
        settings.verbose = 1;
    scene_init();
    decor_init();
    input_place_cursor(screen_w / 2, screen_h / 2);
    frame_fd = timerfd_create(TFD_NONBLOCK | TFD_CLOEXEC);
    struct timerfd_spec spec = { FRAME_MS, FRAME_MS };
    timerfd_settime(frame_fd, &spec);
    scene_damage_all();
    report_at = uptime_ms() + 10000;
    settings.display_mode = DISPLAY_MODE_PACK(screen_w * screen_scale, screen_h * screen_scale, screen_scale);
    if (screen_scale > 1)
        comp_log("started %dx%d scale %d", screen_w, screen_h, screen_scale);
    else
        comp_log("started %dx%d", screen_w, screen_h);
    while (running) {
        struct pollfd pf[OPEN_MAX];
        int n = 0;
        pf[n++] = (struct pollfd){ wire_server_fd(srv), POLLIN, 0 };
        pf[n++] = (struct pollfd){ frame_fd, POLLIN, 0 };
        int input_index = n;
        int ninput = input_fill_pollfds(pf + n, 16);
        n += ninput;
        int fetch_index = -1;
        if (data_fetch_fd() >= 0) {
            fetch_index = n;
            pf[n++] = (struct pollfd){ data_fetch_fd(), POLLIN, 0 };
        }
        struct wire_client *clients[OPEN_MAX];
        int nclients = 0;
        for (struct wire_client *c = wire_server_first_client(srv); c && n < OPEN_MAX; c = wire_client_next(c)) {
            clients[nclients++] = c;
            pf[n++] = (struct pollfd){ wire_client_fd(c), POLLIN, 0 };
        }
        int r = poll(pf, (unsigned)n, 1000);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pf[0].revents & POLLIN) {
            struct wire_client *c = wire_server_accept(srv);
            if (c)
                client_attach(c);
        }
        input_handle(pf + input_index, ninput);
        if (fetch_index >= 0 && (pf[fetch_index].revents & (POLLIN | POLLHUP)))
            data_fetch_read();
        int first_client = fetch_index >= 0 ? fetch_index + 1 : input_index + ninput;
        for (int i = 0; i < nclients; i++)
            if (pf[first_client + i].revents & (POLLIN | POLLHUP)) {
                if (wire_client_dispatch(clients[i]) < 0)
                    wire_client_destroy(clients[i]);
            }
        flush_clients();
        if (pf[1].revents & POLLIN)
            frame();
    }
    scene_stats();
    wire_server_destroy(srv);
    input_close();
    backend_release();
    comp_log("stopped");
    if (logfile)
        fclose(logfile);
    return 0;
}
