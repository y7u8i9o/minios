/* The input core: key state, repeat, grabs and reader queues, the PS/2
 * drivers reporting to it, and the virtio keyboard. */
#include <tests/ktest.h>
#include <input/input.h>
#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>
#include <drivers/tty.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <console.h>
#include <errno.h>

static struct file *open_device(struct input_dev *d)
{
    char path[32];
    ksnprintf(path, sizeof path, "/dev/input/event%u", d->index);
    struct file *f;
    ktest_assert(vfs_open(path, O_RDONLY | O_NONBLOCK, 0, &f) == 0, "open %s", path);
    return f;
}

/* Read every queued event; returns the count. */
static int read_events(struct file *f, struct input_event *ev, int max)
{
    int n = 0;
    while (n < max) {
        long r = file_read(f, (char *)(ev + n), (size_t)(max - n) * sizeof ev[0]);
        if (r <= 0)
            break;
        n += (int)(r / (long)sizeof ev[0]);
    }
    return n;
}

static void expect_event(const struct input_event *e, uint16_t type, uint16_t code, int32_t value, const char *what)
{
    ktest_assert(e->type == type && e->code == code && e->value == value, "%s: got %u %u %d, expected %u %u %d",
                 what, e->type, e->code, e->value, type, code, value);
}

static void drain_console(void)
{
    while (tty_getc(&console_tty) >= 0)
        ;
}

static void press_release(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
}

static void test_input(void)
{
    struct input_dev *kbd = ps2kbd_device();
    ktest_assert(kbd->evbit & (1u << EV_REP), "keyboard does not repeat");
    struct file *f = open_device(kbd);
    struct input_event ev[64];
    ktest_assert(file_read(f, (char *)ev, 4) == -EINVAL, "short read rejected");
    ktest_assert(file_read(f, (char *)ev, sizeof ev) == -EAGAIN, "empty queue does not return EAGAIN");

    /* A press, the same make code again (host repeat) and the release:
     * one press and one release event. */
    ps2kbd_feed_scancode(0x1e);
    ps2kbd_feed_scancode(0x1e);
    ps2kbd_feed_scancode(0x9e);
    int n = read_events(f, ev, 64);
    ktest_assert(n == 4, "press and release: %d events", n);
    expect_event(&ev[0], EV_KEY, KEY_A, 1, "press");
    expect_event(&ev[1], EV_SYN, SYN_REPORT, 0, "sync");
    expect_event(&ev[2], EV_KEY, KEY_A, 0, "release");
    expect_event(&ev[3], EV_SYN, SYN_REPORT, 0, "sync");
    ktest_assert(ev[1].time_us >= ev[0].time_us && ev[2].time_us >= ev[1].time_us, "timestamps not monotonic");

    /* Several keys down at once, released in another order. */
    ps2kbd_feed_scancode(0x2a);           /* left shift */
    ps2kbd_feed_scancode(0x1e);           /* a */
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x48);           /* up */
    ktest_assert(input_key_down(kbd, KEY_LEFTSHIFT) && input_key_down(kbd, KEY_A) && input_key_down(kbd, KEY_UP),
                 "three keys not down together");
    ps2kbd_feed_scancode(0x9e);
    ktest_assert(input_key_down(kbd, KEY_LEFTSHIFT) && !input_key_down(kbd, KEY_A) && input_key_down(kbd, KEY_UP),
                 "release of one key changed the others");
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xc8);
    ps2kbd_feed_scancode(0xaa);
    n = read_events(f, ev, 64);
    ktest_assert(n == 12, "rollover: %d events", n);
    expect_event(&ev[4], EV_KEY, KEY_UP, 1, "extended press");
    expect_event(&ev[6], EV_KEY, KEY_A, 0, "release a");
    expect_event(&ev[10], EV_KEY, KEY_LEFTSHIFT, 0, "release shift");

    /* The console handler: both shifts count, caps lock, control. */
    uint32_t saved = tty_get_lflag(&console_tty);
    tty_set_lflag(&console_tty, 0);
    drain_console();
    ps2kbd_feed_scancode(0x2a);
    ps2kbd_feed_scancode(0x36);
    ps2kbd_feed_scancode(0xaa);           /* left shift up, right still down */
    press_release(0x1e);
    ps2kbd_feed_scancode(0xb6);
    press_release(0x1e);
    ps2kbd_feed_scancode(0x3a);
    ps2kbd_feed_scancode(0xba);           /* caps lock */
    press_release(0x1e);
    ps2kbd_feed_scancode(0x3a);
    ps2kbd_feed_scancode(0xba);
    ps2kbd_feed_scancode(0x1d);
    press_release(0x1e);                  /* control a */
    ps2kbd_feed_scancode(0x9d);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x48);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xc8);           /* up: an escape sequence in raw mode */
    static const char want[] = "AaA\001\033[A";
    for (const char *p = want; *p; p++) {
        int c = tty_getc(&console_tty);
        ktest_assert(c == (uint8_t)*p, "console got %d, expected %d", c, *p);
    }
    ktest_assert(tty_getc(&console_tty) < 0, "console has extra bytes");

    /* Software repeat: a pressed key repeats after the delay, at the
     * period, and the console sees the repeats as presses. */
    uint32_t rep[REP_CNT] = { 200, 50 };
    spin_lock(&kbd->lock);
    memcpy(kbd->rep, rep, sizeof rep);
    spin_unlock(&kbd->lock);
    read_events(f, ev, 64);
    ps2kbd_feed_scancode(0x30);           /* b */
    sleep_ms(100);
    n = read_events(f, ev, 64);
    ktest_assert(n == 2, "repeat before the delay: %d events", n);
    sleep_ms(400);
    ps2kbd_feed_scancode(0xb0);
    n = read_events(f, ev, 64);
    int repeats = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].type == EV_KEY && ev[i].value == 2)
            repeats++;
    ktest_assert(repeats >= 3 && repeats <= 8, "%d repeats in 500 ms at 200/50", repeats);
    expect_event(&ev[n - 2], EV_KEY, KEY_B, 0, "release after repeats");
    int chars = 0;
    while (tty_getc(&console_tty) == 'b')
        chars++;
    ktest_assert(chars == repeats + 1, "console got %d characters for %d repeats", chars, repeats);
    rep[REP_DELAY] = PS2KBD_REPEAT_DELAY_MS;
    rep[REP_PERIOD] = PS2KBD_REPEAT_PERIOD_MS;
    spin_lock(&kbd->lock);
    memcpy(kbd->rep, rep, sizeof rep);
    spin_unlock(&kbd->lock);

    /* A grab: only the grabbing reader sees the keys, the console none;
     * closing the descriptor drops the grab. */
    struct file *g = open_device(kbd);
    ktest_assert(g->ops->ioctl(g, EVIOCGRAB, 1) == 0, "grab failed");
    ktest_assert(f->ops->ioctl(f, EVIOCGRAB, 1) == -EBUSY, "second grab not refused");
    ktest_assert(input_dev_grabbed(kbd), "device not reported grabbed");
    press_release(0x1e);
    n = read_events(f, ev, 64);
    ktest_assert(n == 0, "ungrabbed reader got %d events during the grab", n);
    n = read_events(g, ev, 64);
    ktest_assert(n == 4, "grabbing reader got %d events", n);
    ktest_assert(tty_getc(&console_tty) < 0, "console got a key during the grab");
    file_put(g);
    ktest_assert(!input_dev_grabbed(kbd), "grab survived the close");
    press_release(0x1e);
    n = read_events(f, ev, 64);
    ktest_assert(n == 4, "reader got %d events after the grab", n);
    ktest_assert(tty_getc(&console_tty) == 'a', "console got nothing after the grab");
    tty_set_lflag(&console_tty, saved);

    /* Overflow: the queue restarts with SYN_DROPPED. */
    for (int i = 0; i < 300; i++)
        press_release(0x1e);
    n = read_events(f, ev, 64);
    expect_event(&ev[0], EV_SYN, SYN_DROPPED, 0, "overflow marker");
    while (read_events(f, ev, 64) > 0)
        ;
    file_put(f);
    kprintf("input: core ok\n");
}
KTEST_DEFINE("input", test_input);

static void feed_packet_wheel(uint8_t flags, int dx, int dy, int dz)
{
    ps2mouse_feed_byte((uint8_t)(0x08 | flags | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0)));
    ps2mouse_feed_byte((uint8_t)dx);
    ps2mouse_feed_byte((uint8_t)dy);
    if (ps2mouse_has_wheel())
        ps2mouse_feed_byte((uint8_t)(dz & 0x0f));
}

static void feed_packet(uint8_t flags, int dx, int dy)
{
    feed_packet_wheel(flags, dx, dy, 0);
}

/* PS/2 packets become relative motion and button events. */
static void test_mouse(void)
{
    struct input_dev *m = ps2mouse_device();
    ktest_assert((m->relbit & (1u << REL_X)) && (m->keybit[BTN_LEFT / 8] & (1u << (BTN_LEFT % 8))),
                 "mouse capabilities");
    struct file *f = open_device(m);
    struct input_event ev[32];
    ps2mouse_feed_byte(0x00);                 /* garbage without the sync bit */
    feed_packet(0x01, 5, 3);                  /* left button, right and up */
    feed_packet(0x00, -2, -7);                /* release, left and down */
    feed_packet(0x40, 1, 1);                  /* overflow, dropped */
    feed_packet(0x04, 0, 0);                  /* middle button */
    int n = read_events(f, ev, 32);
    ktest_assert(n == 10, "read %d events", n);
    expect_event(&ev[0], EV_REL, REL_X, 5, "dx");
    expect_event(&ev[1], EV_REL, REL_Y, -3, "dy");
    expect_event(&ev[2], EV_KEY, BTN_LEFT, 1, "left down");
    expect_event(&ev[3], EV_SYN, SYN_REPORT, 0, "sync");
    expect_event(&ev[4], EV_REL, REL_X, -2, "dx");
    expect_event(&ev[5], EV_REL, REL_Y, 7, "dy");
    expect_event(&ev[6], EV_KEY, BTN_LEFT, 0, "left up");
    expect_event(&ev[7], EV_SYN, SYN_REPORT, 0, "sync");
    expect_event(&ev[8], EV_KEY, BTN_MIDDLE, 1, "middle down");
    expect_event(&ev[9], EV_SYN, SYN_REPORT, 0, "sync");
    feed_packet(0x00, 0, 0);
    n = read_events(f, ev, 32);
    ktest_assert(n == 2 && ev[0].code == BTN_MIDDLE && ev[0].value == 0, "middle release: %d events", n);
    file_put(f);
    kprintf("mouse: events ok\n");
}
KTEST_DEFINE("mouse", test_mouse);

/* Four byte IntelliMouse packets carry the wheel; the packet length
 * follows the detected device. */
static void test_mouse_wheel(void)
{
    bool had_wheel = ps2mouse_has_wheel();
    ktest_assert(had_wheel, "QEMU's mouse did not report id 3");
    struct file *f = open_device(ps2mouse_device());
    struct input_event ev[32];
    feed_packet_wheel(0x00, 0, 0, 1);         /* wheel towards the user */
    feed_packet_wheel(0x02, 3, -1, -1);       /* right button, wheel away */
    feed_packet_wheel(0x00, 0, 0, -8);        /* largest negative delta */
    int n = read_events(f, ev, 32);
    ktest_assert(n == 10, "read %d events", n);
    expect_event(&ev[0], EV_REL, REL_WHEEL, -1, "wheel towards the user");
    expect_event(&ev[2], EV_REL, REL_X, 3, "dx");
    expect_event(&ev[3], EV_REL, REL_Y, 1, "dy");
    expect_event(&ev[4], EV_REL, REL_WHEEL, 1, "wheel away");
    expect_event(&ev[5], EV_KEY, BTN_RIGHT, 1, "right down");
    expect_event(&ev[7], EV_REL, REL_WHEEL, 8, "large wheel delta");
    expect_event(&ev[8], EV_KEY, BTN_RIGHT, 0, "right up");
    /* A device without a wheel retains three byte packets. */
    ps2mouse_set_wheel(false);
    feed_packet(0x01, 2, 2);
    n = read_events(f, ev, 32);
    ktest_assert(n == 4, "three byte packet: %d events", n);
    expect_event(&ev[0], EV_REL, REL_X, 2, "dx");
    expect_event(&ev[1], EV_REL, REL_Y, -2, "dy");
    expect_event(&ev[2], EV_KEY, BTN_LEFT, 1, "left down");
    ps2mouse_set_wheel(had_wheel);
    feed_packet_wheel(0x02, 0, 0, 0);
    read_events(f, ev, 32);
    file_put(f);
    kprintf("mouse_wheel: wheel events ok\n");
}
KTEST_DEFINE("mouse_wheel", test_mouse_wheel);

/* A virtio keyboard probes with the key capabilities and repeat, and
 * its keys reach the console like PS/2 keys. */
static void test_input_keyboard(void)
{
    struct input_dev *k = input_device_by_name("QEMU Virtio Keyboard");
    ktest_assert(k != NULL, "no virtio keyboard device");
    ktest_assert((k->evbit & (1u << EV_KEY)) && (k->evbit & (1u << EV_REP)), "keyboard capabilities %x", k->evbit);
    ktest_assert(k->keybit[KEY_A / 8] & (1u << (KEY_A % 8)), "KEY_A missing");
    ktest_assert(k->keybit[KEY_UP / 8] & (1u << (KEY_UP % 8)), "KEY_UP missing");
    ktest_assert(k->id.bustype == BUS_VIRTIO || k->id.bustype == BUS_VIRTUAL, "bus type %x", k->id.bustype);
    uint32_t saved = tty_get_lflag(&console_tty);
    tty_set_lflag(&console_tty, 0);
    drain_console();
    input_report_key(k, KEY_LEFTSHIFT, 1);
    input_sync(k);
    input_report_key(k, KEY_H, 1);
    input_sync(k);
    input_report_key(k, KEY_H, 0);
    input_sync(k);
    input_report_key(k, KEY_LEFTSHIFT, 0);
    input_sync(k);
    input_report_key(k, KEY_I, 1);
    input_sync(k);
    input_report_key(k, KEY_I, 0);
    input_sync(k);
    ktest_assert(tty_getc(&console_tty) == 'H' && tty_getc(&console_tty) == 'i', "console did not get Hi");
    tty_set_lflag(&console_tty, saved);
    kprintf("input_keyboard: virtio keyboard ok\n");
}
KTEST_DEFINE("input_keyboard", test_input_keyboard);
