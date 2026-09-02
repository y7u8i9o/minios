#define KLOG_SUBSYS "ps2mouse"
#include <drivers/ps2mouse.h>
#include <drivers/mouse.h>
#include <sync/spinlock.h>
#include <arch/apic.h>
#include <arch/irq.h>
#include <arch/io.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

/* Packet assembly. Protected by mouse_lock, taken in the interrupt
 * handler; events go to the ring in drivers/mouse.c. */
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
    if (mouse.npacket == mouse.packet_len) {
        mouse.npacket = 0;
        uint8_t f = mouse.packet[0];
        if (!(f & 0xc0)) {   /* drop overflow packets */
            int dx = mouse.packet[1] - ((f & 0x10) ? 256 : 0);
            int dy = mouse.packet[2] - ((f & 0x20) ? 256 : 0);
            /* IntelliMouse: the low nibble of byte 3 is the signed wheel delta. */
            int dz = mouse.packet_len == 4 ? (int)(mouse.packet[3] & 0x0f) : 0;
            if (dz & 0x08)
                dz -= 16;
            struct mouse_event e = {
                .dx = (int16_t)dx,
                .dy = (int16_t)-dy,
                .buttons = f & 7,
                .dz = (int8_t)dz,
            };
            mouse_push(&e);
        }
    }
    spin_unlock(&mouse_lock);
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
    irq_register(IRQ_MOUSE, mouse_irq, NULL);
    ioapic_route(GSI_MOUSE, IRQ_MOUSE, false);
    klog_info("ps/2 mouse on irq %u%s%s", IRQ_MOUSE, mouse.packet_len == 4 ? " with wheel" : "",
              mouse.present ? "" : " (no acknowledgement)");
}
