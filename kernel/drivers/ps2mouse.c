/* The PS/2 mouse: packets become relative motion, button and wheel events
 * of an input core device. The 8042 controller that probes the mouse and
 * delivers the bytes is platform code (arch/x86_64/i8042.c). */
#define KLOG_SUBSYS "ps2mouse"
#include <drivers/ps2mouse.h>
#include <input/input.h>
#include <sync/spinlock.h>
#include <klog.h>

static struct input_dev ps2mouse_dev;

/* Packet assembly. Protected by mouse_lock, taken in the interrupt
 * handler; complete packets are reported to the input core. */
static DEFINE_SPINLOCK(mouse_lock);
static struct {
    uint8_t packet[4];
    unsigned npacket;
    unsigned packet_len;        /* 3, or 4 with a wheel */
    bool present;
} mouse;

void ps2mouse_feed_byte(uint8_t b)
{
    spin_lock(&mouse_lock);
    /* Byte 0 always has bit 3 set; use it to resynchronize. */
    if (mouse.npacket == 0 && !(b & 0x08)) {
        spin_unlock(&mouse_lock);
        return;
    }
    mouse.packet[mouse.npacket++] = b;
    if (mouse.npacket < mouse.packet_len) {
        spin_unlock(&mouse_lock);
        return;
    }
    mouse.npacket = 0;
    uint8_t f = mouse.packet[0];
    int dx = mouse.packet[1] - ((f & 0x10) ? 256 : 0);
    int dy = mouse.packet[2] - ((f & 0x20) ? 256 : 0);
    /* IntelliMouse: the low nibble of byte 3 is the signed wheel delta,
     * positive towards the user. */
    int dz = mouse.packet_len == 4 ? (int)(mouse.packet[3] & 0x0f) : 0;
    if (dz & 0x08)
        dz -= 16;
    bool overflow = (f & 0xc0) != 0;
    spin_unlock(&mouse_lock);
    if (overflow)
        return;
    struct input_dev *d = &ps2mouse_dev;
    /* The packet counts y upwards, REL_Y counts downwards; REL_WHEEL
     * counts away from the user. */
    input_report_rel(d, REL_X, dx);
    input_report_rel(d, REL_Y, -dy);
    input_report_rel(d, REL_WHEEL, -dz);
    input_report_key(d, BTN_LEFT, f & 1);
    input_report_key(d, BTN_RIGHT, f & 2);
    input_report_key(d, BTN_MIDDLE, f & 4);
    input_sync(d);
}

bool ps2mouse_has_wheel(void)
{
    spin_lock(&mouse_lock);
    bool r = mouse.packet_len == 4;
    spin_unlock(&mouse_lock);
    return r;
}

void ps2mouse_set_wheel(bool wheel)
{
    spin_lock(&mouse_lock);
    mouse.packet_len = wheel ? 4 : 3;
    mouse.npacket = 0;
    spin_unlock(&mouse_lock);
}

struct input_dev *ps2mouse_device(void)
{
    return &ps2mouse_dev;
}

void ps2mouse_register(unsigned packet_len, bool present)
{
    spin_lock(&mouse_lock);
    mouse.packet_len = packet_len;
    mouse.present = present;
    spin_unlock(&mouse_lock);
    input_dev_init(&ps2mouse_dev, packet_len == 4 ? "ImPS/2 Generic Wheel Mouse" : "PS/2 Generic Mouse",
                   BUS_I8042);
    input_set_rel_cap(&ps2mouse_dev, REL_X);
    input_set_rel_cap(&ps2mouse_dev, REL_Y);
    input_set_rel_cap(&ps2mouse_dev, REL_WHEEL);
    input_set_key_cap(&ps2mouse_dev, BTN_LEFT);
    input_set_key_cap(&ps2mouse_dev, BTN_RIGHT);
    input_set_key_cap(&ps2mouse_dev, BTN_MIDDLE);
    input_register_device(&ps2mouse_dev);
}
