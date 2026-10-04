/* D2: USB keyboards, mice and tablets behind an xHCI controller
 * (docs/design/usb.md). The case attaches usb-kbd, usb-tablet and usb-mouse
 * and no virtio input device. When this test prints that the devices are
 * ready, tests/qmp_input.py sends the key a, the absolute position
 * 10000,20000, a click of the left button and the relative movement 5,-3
 * through QMP. QEMU routes the button to the tablet or to the mouse,
 * whichever its input core selected last, so either may report it. */
#include <tests/ktest.h>
#include <input/input.h>
#include <drivers/usb.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <lib/printf.h>
#include <console.h>
#include <errno.h>

struct received {
    bool key_down, key_up, button_down, button_up;
    bool have_x, have_y;
    int32_t x, y, dx, dy;
};

static struct input_dev *find_usb(const char *name)
{
    struct input_dev *d = input_device_by_name(name);
    ktest_assert(d, "no input device %s", name);
    ktest_assert(d->id.bustype == BUS_USB, "%s has bus type %x", name, d->id.bustype);
    return d;
}

static struct file *open_device(struct input_dev *d)
{
    char path[32];
    ksnprintf(path, sizeof path, "/dev/input/event%u", d->index);
    struct file *f;
    ktest_assert(vfs_open(path, O_RDONLY | O_NONBLOCK, 0, &f) == 0, "open %s", path);
    return f;
}

static void drain(struct file *f, struct received *r)
{
    struct input_event ev[32];
    for (;;) {
        long n = file_read(f, (char *)ev, sizeof ev);
        if (n <= 0)
            return;
        for (long i = 0; i < n / (long)sizeof ev[0]; i++) {
            const struct input_event *e = &ev[i];
            if (e->type == EV_KEY && e->code == KEY_A && e->value == 1)
                r->key_down = true;
            else if (e->type == EV_KEY && e->code == KEY_A && e->value == 0)
                r->key_up = true;
            else if (e->type == EV_KEY && e->code == BTN_LEFT && e->value == 1)
                r->button_down = true;
            else if (e->type == EV_KEY && e->code == BTN_LEFT && e->value == 0)
                r->button_up = true;
            else if (e->type == EV_ABS && e->code == ABS_X)
                r->x = e->value, r->have_x = true;
            else if (e->type == EV_ABS && e->code == ABS_Y)
                r->y = e->value, r->have_y = true;
            else if (e->type == EV_REL && e->code == REL_X)
                r->dx += e->value;
            else if (e->type == EV_REL && e->code == REL_Y)
                r->dy += e->value;
        }
    }
}

static void test_usb_hid(void)
{
    ktest_assert(usb_device_count() == 3, "%u USB devices, expected 3", usb_device_count());
    struct input_dev *kbd = find_usb("QEMU USB Keyboard");
    struct input_dev *tablet = find_usb("QEMU USB Tablet");
    struct input_dev *mouse = find_usb("QEMU USB Mouse");
    ktest_assert(kbd->keybit[KEY_A / 8] & (1u << (KEY_A % 8)), "the keyboard lacks KEY_A");
    ktest_assert(kbd->evbit & (1u << EV_REP), "the keyboard does not repeat");
    ktest_assert((tablet->absbit & 3) == 3 && tablet->abs[ABS_X].maximum == 32767,
                 "the tablet has absbit %x and an X maximum of %d", tablet->absbit, tablet->abs[ABS_X].maximum);
    ktest_assert((mouse->relbit & 3) == 3, "the mouse has relbit %x", mouse->relbit);
    struct file *fk = open_device(kbd), *ft = open_device(tablet), *fm = open_device(mouse);

    kprintf("usb: devices ready\n");
    struct received r = { 0 };
    uint64_t end = timer_ms() + 30000;
    while (timer_ms() < end) {
        drain(fk, &r);
        drain(ft, &r);
        drain(fm, &r);
        if (r.key_down && r.key_up && r.have_x && r.have_y && r.x == 10000 && r.y == 20000 &&
            r.button_down && r.button_up && r.dx == 5 && r.dy == -3)
            break;
        sleep_ms(20);
    }
    ktest_assert(r.key_down && r.key_up, "key a: press %d, release %d", r.key_down, r.key_up);
    ktest_assert(r.have_x && r.have_y && r.x == 10000 && r.y == 20000, "absolute position %d,%d", r.x, r.y);
    ktest_assert(r.button_down && r.button_up, "left button: press %d, release %d", r.button_down, r.button_up);
    ktest_assert(r.dx == 5 && r.dy == -3, "relative movement %d,%d", r.dx, r.dy);
    file_put(fk);
    file_put(ft);
    file_put(fm);
    kprintf("usb: key, absolute position %d,%d, button and relative movement %d,%d received\n", r.x, r.y, r.dx,
            r.dy);
}
KTEST_DEFINE("usb_hid", test_usb_hid);
