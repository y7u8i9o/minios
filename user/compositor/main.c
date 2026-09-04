/* X12: a listening socket, clients, the input devices and a
 * 60 Hz frame clock in one poll loop. Every notable event is logged as
 * "x12: ..." for the tests. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
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
static int mouse_fd, kbd_fd, frame_fd;
static struct termios saved_kbd;
int cursor_x, cursor_y;

void comp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("x12: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

uint32_t comp_serial(void) { return serial++; }

static void on_term(int sig) { running = 0; }

static int buttons;

static void handle_mouse(void)
{
    struct mouse_event ev[8];
    ssize_t n = read(mouse_fd, ev, sizeof ev);
    for (ssize_t i = 0; i < n / (ssize_t)sizeof ev[0]; i++) {
        if (ev[i].flags & MOUSE_ABSOLUTE) {
            /* Tablets: the position maps onto the screen. */
            int x = (int)((long)ev[i].ax * screen_w / (MOUSE_ABS_MAX + 1));
            int y = (int)((long)ev[i].ay * screen_h / (MOUSE_ABS_MAX + 1));
            if (x != cursor_x || y != cursor_y) {
                cursor_x = x;
                cursor_y = y;
                scene_set_cursor(cursor_x, cursor_y);
                seat_pointer_motion();
            }
        } else if (ev[i].dx || ev[i].dy) {
            cursor_x += ev[i].dx;
            cursor_y += ev[i].dy;
            if (cursor_x < 0) cursor_x = 0;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_x >= screen_w) cursor_x = screen_w - 1;
            if (cursor_y >= screen_h) cursor_y = screen_h - 1;
            scene_set_cursor(cursor_x, cursor_y);
            seat_pointer_motion();
        }
        if (ev[i].dz)
            seat_pointer_axis(ev[i].dz);
        int pressed = ev[i].buttons & ~buttons, released = buttons & ~ev[i].buttons;
        buttons = ev[i].buttons;
        for (int b = 0; b < 3; b++) {
            if (pressed & (1 << b))
                seat_pointer_button(b + 1, 1);
            if (released & (1 << b))
                seat_pointer_button(b + 1, 0);
        }
    }
}

static void handle_keyboard(void)
{
    static int extended;
    uint8_t bytes[32];
    ssize_t n = read(kbd_fd, bytes, sizeof bytes);
    for (ssize_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        if (b == 0xe0) {
            extended = 1;
            continue;
        }
        uint32_t key = (b & 0x7f) | (extended ? 0x80 : 0);
        extended = 0;
        seat_key(key, !(b & 0x80));
    }
}

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
        if (settings.verbose)
            comp_log("frame");
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
            comp_log("%ld frames in the last %ld s", frames_since_report, (now - (report_at - 10000)) / 1000);
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
    if (cursor_x >= screen_w) cursor_x = screen_w - 1;
    if (cursor_y >= screen_h) cursor_y = screen_h - 1;
    scene_set_cursor(cursor_x, cursor_y);
    shell_output_changed();
    output_changed();
    scene_damage_all();
    comp_log("mode %dx%d scale %d", screen_w, screen_h, screen_scale);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);           /* a dead client must not kill the compositor */
    if (backend_init() < 0) {
        perror("x12: framebuffer");
        return 1;
    }
    mouse_fd = open("/dev/mouse", O_RDONLY | O_CLOEXEC);
    kbd_fd = open("/dev/kbd", O_RDONLY | O_CLOEXEC);
    if (mouse_fd < 0 || kbd_fd < 0) {
        perror("x12: input devices");
        return 1;
    }
    tcgetattr(kbd_fd, &saved_kbd);
    struct termios raw = { .c_lflag = KBD_SCANCODES };
    tcsetattr(kbd_fd, TCSANOW, &raw);
    srv = wire_server_create(argc > 1 ? argv[1] : "display");
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
    scene_init();
    decor_init();
    cursor_x = screen_w / 2;
    cursor_y = screen_h / 2;
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
        pf[n++] = (struct pollfd){ mouse_fd, POLLIN, 0 };
        pf[n++] = (struct pollfd){ kbd_fd, POLLIN, 0 };
        pf[n++] = (struct pollfd){ frame_fd, POLLIN, 0 };
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
        if (pf[1].revents & POLLIN)
            handle_mouse();
        if (pf[2].revents & POLLIN)
            handle_keyboard();
        if (fetch_index >= 0 && (pf[fetch_index].revents & (POLLIN | POLLHUP)))
            data_fetch_read();
        int first_client = fetch_index >= 0 ? fetch_index + 1 : 4;
        for (int i = 0; i < nclients; i++)
            if (pf[first_client + i].revents & (POLLIN | POLLHUP)) {
                if (wire_client_dispatch(clients[i]) < 0)
                    wire_client_destroy(clients[i]);
            }
        flush_clients();
        if (pf[3].revents & POLLIN)
            frame();
    }
    scene_stats();
    wire_server_destroy(srv);
    tcsetattr(kbd_fd, TCSANOW, &saved_kbd);
    backend_release();
    comp_log("stopped");
    return 0;
}
