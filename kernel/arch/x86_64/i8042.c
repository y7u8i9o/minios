/* The 8042 controller of the PC: the PS/2 keyboard on its first port and
 * the PS/2 mouse on its second. The bytes it delivers are decoded by
 * drivers/ps2kbd.c and drivers/ps2mouse.c. */
#define KLOG_SUBSYS "i8042"
#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>
#include <arch/apic.h>
#include <arch/irq.h>
#include <arch/io.h>
#include <klog.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

static void kbd_irq(struct trapframe *tf, void *arg)
{
    while (inb(PS2_STATUS) & 0x01) {
        uint8_t st = inb(PS2_STATUS);
        uint8_t b = inb(PS2_DATA);
        if (st & 0x20)
            ps2mouse_feed_byte(b);
        else
            ps2kbd_feed_scancode(b);
    }
}

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

/* Set the sample rate; part of the IntelliMouse enabling sequence. */
static bool mouse_set_rate(uint8_t rate)
{
    return mouse_cmd(0xf3) == 0xfa && mouse_cmd(rate) == 0xfa;
}

void ps2kbd_init(void)
{
    ps2kbd_register();
    /* Drain anything pending, then unmask the line. */
    while (inb(PS2_STATUS) & 0x01)
        (void)inb(PS2_DATA);
    irq_register(IRQ_KEYBOARD, kbd_irq, NULL);
    ioapic_route(GSI_KEYBOARD, IRQ_KEYBOARD, false);
    klog_info("ps/2 keyboard on irq %u", IRQ_KEYBOARD);
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
    unsigned packet_len = 3;
    /* The IntelliMouse sequence: sample rates 200, 100, 80, then the
     * device reports id 3 if it has a wheel and switches to four byte
     * packets. Devices without a wheel keep id 0 and three byte packets. */
    if (a1 == 0xfa && mouse_set_rate(200) && mouse_set_rate(100) && mouse_set_rate(80) &&
        mouse_cmd(0xf2) == 0xfa) {
        wait_output_full();
        uint8_t id = inb(PS2_DATA);
        if (id == 3)
            packet_len = 4;
    }
    uint8_t a2 = mouse_cmd(0xf4);    /* enable reporting */
    bool present = a1 == 0xfa && a2 == 0xfa;
    ps2mouse_register(packet_len, present);

    irq_register(IRQ_MOUSE, mouse_irq, NULL);
    ioapic_route(GSI_MOUSE, IRQ_MOUSE, false);
    klog_info("ps/2 mouse on irq %u%s%s", IRQ_MOUSE, packet_len == 4 ? " with wheel" : "",
              present ? "" : " (no acknowledgement)");
}
