/* The PS/2 mouse on the second 8042 port: packets become relative
 * motion, button and wheel events of an input core device. */
#define KLOG_SUBSYS "ps2mouse"
#include <drivers/ps2mouse.h>
#include <input/input.h>
#include <sync/spinlock.h>
#include <arch/apic.h>
#include <arch/irq.h>
#include <arch/io.h>
#include <klog.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

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

static void wait_input_clear(void)
{
    for (int i = 0; i < 100000 && (inb(PS2_STATUS) & 0x02); i++)
        io_wait();
}

static void wait_output_full(void)
{
    for (int i = 0; i < 100000 && !(inb(PS2_STATUS) & 0x01); i++)
        io_wait();
}

static void ctl_write(uint8_t cmd)
{
    wait_input_clear();
    outb(PS2_CMD, cmd);
}

/* Send a byte to the mouse and return its acknowledgement. */
static uint8_t mouse_cmd(uint8_t b)
{
    ctl_write(0xd4);
    wait_input_clear();
    outb(PS2_DATA, b);
    wait_output_full();
    return inb(PS2_DATA);
}

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

static void mouse_irq(struct trapframe *tf, void *arg)
{
    /* Both 8042 ports raise data on the same register; bit 5 of the
     * status selects the auxiliary device. */
    while (inb(PS2_STATUS) & 0x01) {
        uint8_t st = inb(PS2_STATUS);
        uint8_t b = inb(PS2_DATA);
        if (st & 0x20)
            ps2mouse_feed_byte(b);
    }
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

/* Set the sample rate; part of the IntelliMouse enabling sequence. */
static bool mouse_set_rate(uint8_t rate)
{
    return mouse_cmd(0xf3) == 0xfa && mouse_cmd(rate) == 0xfa;
}

void ps2mouse_init(void)
{
    ctl_write(0xa8);                 /* enable the auxiliary port */
    ctl_write(0x20);                 /* read the command byte */
    wait_output_full();
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;                     /* auxiliary interrupt */
    cfg &= (uint8_t)~0x20;           /* auxiliary clock enabled */
    ctl_write(0x60);
    wait_input_clear();
    outb(PS2_DATA, cfg);
    uint8_t a1 = mouse_cmd(0xf6);    /* defaults */
    mouse.packet_len = 3;
    /* The IntelliMouse sequence: sample rates 200, 100, 80, then the
     * device reports id 3 if it has a wheel and switches to four byte
     * packets. Devices without a wheel keep id 0 and three byte packets. */
    if (a1 == 0xfa && mouse_set_rate(200) && mouse_set_rate(100) && mouse_set_rate(80) &&
        mouse_cmd(0xf2) == 0xfa) {
        wait_output_full();
        uint8_t id = inb(PS2_DATA);
        if (id == 3)
            mouse.packet_len = 4;
    }
    uint8_t a2 = mouse_cmd(0xf4);    /* enable reporting */
    mouse.present = a1 == 0xfa && a2 == 0xfa;

    input_dev_init(&ps2mouse_dev, mouse.packet_len == 4 ? "ImPS/2 Generic Wheel Mouse" : "PS/2 Generic Mouse",
                   BUS_I8042);
    input_set_rel_cap(&ps2mouse_dev, REL_X);
    input_set_rel_cap(&ps2mouse_dev, REL_Y);
    input_set_rel_cap(&ps2mouse_dev, REL_WHEEL);
    input_set_key_cap(&ps2mouse_dev, BTN_LEFT);
    input_set_key_cap(&ps2mouse_dev, BTN_RIGHT);
    input_set_key_cap(&ps2mouse_dev, BTN_MIDDLE);
    input_register_device(&ps2mouse_dev);

    irq_register(IRQ_MOUSE, mouse_irq, NULL);
    ioapic_route(GSI_MOUSE, IRQ_MOUSE, false);
    klog_info("ps/2 mouse on irq %u%s%s", IRQ_MOUSE, mouse.packet_len == 4 ? " with wheel" : "",
              mouse.present ? "" : " (no acknowledgement)");
}
