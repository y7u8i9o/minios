/* Input devices: every /dev/input/eventN is opened and grabbed at start.
 * Keyboards send key codes to the seat; pointers move the cursor,
 * which is retained in fractions of a logical pixel: tablets place it
 * exactly, mice move it through the acceleration profile below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <minios/input.h>
#include "comp.h"

#define MAX_DEVICES 16

struct idev {
    int fd;
    char name[INPUT_NAME_MAX];
    int keyboard, pointer, absolute;
    struct input_absinfo abs_x, abs_y;
    uint8_t keys[INPUT_KEY_BYTES];      /* keys and buttons down, as delivered */
    /* The report being assembled until SYN_REPORT. */
    int dx, dy, wheel, hwheel, wheel_hi, hwheel_hi, has_hi;
    int ax, ay, has_abs;
    uint64_t last_us;                   /* time of the previous motion report */
};

static struct idev devices[MAX_DEVICES];
static int ndevices;
double cursor_fx, cursor_fy;

static inline int bit(const uint8_t *bits, unsigned n) { return (bits[n / 8] >> (n % 8)) & 1; }

static void set_cursor_position(double fx, double fy)
{
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > screen_w - 0.001) fx = screen_w - 0.001;
    if (fy > screen_h - 0.001) fy = screen_h - 0.001;
    cursor_fx = fx;
    cursor_fy = fy;
    int x = (int)floor(fx), y = (int)floor(fy);
    if (x != cursor_x || y != cursor_y) {
        cursor_x = x;
        cursor_y = y;
        scene_set_cursor(cursor_x, cursor_y);
    }
}

void input_place_cursor(int x, int y)
{
    set_cursor_position(x + 0.5, y + 0.5);
}

/* The acceleration profile. The speed setting (-100..100) scales every
 * motion between 0.2 and 3 times. The adaptive profile also multiplies
 * by a factor of the velocity in device units per millisecond: 0.8 at
 * or below one unit per ms, 2.8 at or above eight, linear in between,
 * so that slow motion is precise and fast motion crosses the screen. */
static double accel_factor(struct idev *d, int dx, int dy, uint64_t t_us)
{
    double s = settings.pointer_speed / 100.0;
    double factor = s < 0 ? 1.0 + s * 0.8 : 1.0 + s * 2.0;
    if (settings.pointer_accel == POINTER_ACCEL_ADAPTIVE) {
        double dt = d->last_us ? (double)(t_us - d->last_us) / 1000.0 : 100.0;
        if (dt < 1.0) dt = 1.0;
        if (dt > 100.0) dt = 100.0;
        double v = sqrt((double)dx * dx + (double)dy * dy) / dt;
        double f;
        if (v <= 1.0)
            f = 0.8;
        else if (v >= 8.0)
            f = 2.8;
        else
            f = 0.8 + (v - 1.0) * (2.0 / 7.0);
        factor *= f;
    }
    d->last_us = t_us;
    return factor;
}

static int button_number(uint16_t code)
{
    switch (code) {
    case BTN_LEFT:   return 1;
    case BTN_RIGHT:  return 2;
    case BTN_MIDDLE: return 3;
    case BTN_SIDE:   return 4;
    case BTN_EXTRA:  return 5;
    }
    return 0;
}

/* A complete report of a pointer device. */
static void pointer_report(struct idev *d, uint64_t t_us)
{
    if (d->has_abs) {
        double rx = (double)(d->abs_x.maximum - d->abs_x.minimum + 1);
        double ry = (double)(d->abs_y.maximum - d->abs_y.minimum + 1);
        set_cursor_position((d->ax - d->abs_x.minimum) * screen_w / rx, (d->ay - d->abs_y.minimum) * screen_h / ry);
        seat_pointer_motion();
    } else if (d->dx || d->dy) {
        double f = accel_factor(d, d->dx, d->dy, t_us);
        set_cursor_position(cursor_fx + d->dx * f, cursor_fy + d->dy * f);
        seat_pointer_motion();
    }
    /* Wheel: one click is 120 high resolution units and scrolls 15
     * logical pixels; positive REL_WHEEL is away from the user, the
     * seat counts positive towards the user. */
    int units = d->has_hi ? d->wheel_hi : d->wheel * 120;
    if (units)
        seat_pointer_axis(-(units * 15 * 256) / 120);
    d->dx = d->dy = d->wheel = d->hwheel = d->wheel_hi = d->hwheel_hi = d->has_hi = 0;
    d->has_abs = 0;
}

static void key_change(struct idev *d, uint16_t code, int down)
{
    if (code >= BTN_MOUSE) {
        int b = button_number(code);
        if (b)
            seat_pointer_button(b, down);
    } else if (d->keyboard) {
        seat_key(code, down);
    }
}

/* Events were lost: fetch the key state and release or press what
 * changed while the queue overflowed. */
static void resync(struct idev *d)
{
    uint8_t now[INPUT_KEY_BYTES];
    if (ioctl(d->fd, EVIOCGKEY, now) < 0)
        return;
    for (unsigned code = 1; code <= KEY_MAX; code++)
        if (bit(d->keys, code) != bit(now, code))
            key_change(d, (uint16_t)code, bit(now, code));
    memcpy(d->keys, now, sizeof now);
    d->dx = d->dy = d->wheel = d->hwheel = d->wheel_hi = d->hwheel_hi = d->has_hi = d->has_abs = 0;
    comp_log("input %s: events dropped, state resynchronized", d->name);
}

static void handle_event(struct idev *d, const struct input_event *e)
{
    switch (e->type) {
    case EV_SYN:
        if (e->code == SYN_DROPPED)
            resync(d);
        else if (d->pointer)
            pointer_report(d, e->time_us);
        break;
    case EV_KEY:
        if (e->value == 2 || e->code > KEY_MAX)
            return;
        if (bit(d->keys, e->code) == (e->value != 0))
            return;
        d->keys[e->code / 8] ^= (uint8_t)(1u << (e->code % 8));
        key_change(d, e->code, e->value != 0);
        break;
    case EV_REL:
        switch (e->code) {
        case REL_X: d->dx += e->value; break;
        case REL_Y: d->dy += e->value; break;
        case REL_WHEEL: d->wheel += e->value; break;
        case REL_HWHEEL: d->hwheel += e->value; break;
        case REL_WHEEL_HI_RES: d->wheel_hi += e->value; d->has_hi = 1; break;
        case REL_HWHEEL_HI_RES: d->hwheel_hi += e->value; d->has_hi = 1; break;
        }
        break;
    case EV_ABS:
        if (e->code == ABS_X) { d->ax = e->value; d->has_abs = 1; }
        else if (e->code == ABS_Y) { d->ay = e->value; d->has_abs = 1; }
        break;
    }
}

static void read_device(struct idev *d)
{
    struct input_event ev[32];
    ssize_t n = read(d->fd, ev, sizeof ev);
    for (ssize_t i = 0; i < n / (ssize_t)sizeof ev[0]; i++)
        handle_event(d, &ev[i]);
}

static int open_device(const char *path)
{
    if (ndevices >= MAX_DEVICES)
        return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct input_caps caps;
    struct idev *d = &devices[ndevices];
    memset(d, 0, sizeof *d);
    if (ioctl(fd, EVIOCGCAPS, &caps) < 0 || ioctl(fd, EVIOCGNAME, d->name) < 0) {
        close(fd);
        return -1;
    }
    d->fd = fd;
    d->keyboard = (caps.ev_bits & (1u << EV_KEY)) && bit(caps.key_bits, KEY_A);
    d->absolute = (caps.abs_bits & (1u << ABS_X)) != 0;
    d->pointer = d->absolute || (caps.rel_bits & (1u << REL_X)) || bit(caps.key_bits, BTN_LEFT);
    if (!d->keyboard && !d->pointer) {
        close(fd);
        return -1;
    }
    if (d->absolute && (ioctl(fd, EVIOCGABS(ABS_X), &d->abs_x) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &d->abs_y) < 0)) {
        close(fd);
        return -1;
    }
    if (ioctl(fd, EVIOCGRAB, 1) < 0)
        comp_log("input %s: grab refused", d->name);
    ioctl(fd, EVIOCGKEY, d->keys);
    ndevices++;
    comp_log("input %s: %s%s%s", d->name, d->keyboard ? "keyboard" : "",
             d->keyboard && d->pointer ? " and " : "", d->pointer ? (d->absolute ? "absolute pointer" : "pointer") : "");
    return 0;
}

int input_init(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return -1;
    struct dirent *e;
    char names[MAX_DEVICES][16];
    int n = 0;
    while ((e = readdir(dir)) != NULL && n < MAX_DEVICES)
        if (strncmp(e->d_name, "event", 5) == 0)
            strlcpy(names[n++], e->d_name, sizeof names[0]);
    closedir(dir);
    /* In order of the node number, so that event0 is the first keyboard. */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[16];
            memcpy(t, names[j], sizeof t);
            memcpy(names[j], names[j - 1], sizeof t);
            memcpy(names[j - 1], t, sizeof t);
        }
    for (int i = 0; i < n; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/%s", names[i]);
        open_device(path);
    }
    int keyboards = 0, pointers = 0;
    for (int i = 0; i < ndevices; i++) {
        keyboards += devices[i].keyboard;
        pointers += devices[i].pointer;
    }
    return keyboards && pointers ? 0 : -1;
}

int input_fill_pollfds(struct pollfd *pf, int max)
{
    int n = 0;
    for (int i = 0; i < ndevices && n < max; i++)
        pf[n++] = (struct pollfd){ devices[i].fd, POLLIN, 0 };
    return n;
}

void input_handle(const struct pollfd *pf, int n)
{
    for (int i = 0; i < n && i < ndevices; i++)
        if (pf[i].revents & POLLIN)
            read_device(&devices[i]);
}

void input_close(void)
{
    for (int i = 0; i < ndevices; i++)
        close(devices[i].fd);
    ndevices = 0;
}
