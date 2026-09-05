/* The PS/2 keyboard on the first 8042 port: scancode set 1 bytes are
 * translated to key codes and reported to the input core, which keeps
 * the key state, repeats held keys and feeds the console terminal. */
#define KLOG_SUBSYS "ps2kbd"
#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>
#include <input/input.h>
#include <arch/apic.h>
#include <arch/irq.h>
#include <arch/io.h>
#include <sync/spinlock.h>
#include <klog.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64

#define SC_EXTENDED  0xe0
#define SC_PAUSE     0xe1

/* Key codes of the 0xe0 prefixed scancodes (the unprefixed ones 1..88
 * equal their key codes). */
static const uint8_t extended_keys[128] = {
    [0x10] = KEY_PREVIOUSSONG, [0x19] = KEY_NEXTSONG, [0x1c] = KEY_KPENTER, [0x1d] = KEY_RIGHTCTRL,
    [0x20] = KEY_MUTE, [0x22] = KEY_PLAYPAUSE, [0x24] = KEY_STOPCD, [0x2e] = KEY_VOLUMEDOWN,
    [0x30] = KEY_VOLUMEUP, [0x35] = KEY_KPSLASH, [0x37] = KEY_SYSRQ, [0x38] = KEY_RIGHTALT,
    [0x46] = KEY_PAUSE, [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGEUP, [0x4b] = KEY_LEFT,
    [0x4d] = KEY_RIGHT, [0x4f] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGEDOWN, [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE, [0x5b] = KEY_LEFTMETA, [0x5c] = KEY_RIGHTMETA, [0x5d] = KEY_COMPOSE,
    [0x5e] = KEY_POWER, [0x5f] = KEY_SLEEP, [0x63] = KEY_WAKEUP,
};

static struct input_dev ps2kbd_dev;

/* Prefix state of the scancode stream. Protected by kbd_lock, taken in
 * the interrupt handler. */
static DEFINE_SPINLOCK(kbd_lock);
static struct {
    bool extended;
    int pause;                  /* bytes of an 0xe1 sequence still expected */
} kbd;

void ps2kbd_feed_scancode(uint8_t code)
{
    spin_lock(&kbd_lock);
    if (kbd.pause) {
        /* 0xe1 0x1d 0x45 is Pause pressed, 0xe1 0x9d 0xc5 released; the
         * key has no separate release, so a press releases at once. */
        bool press = --kbd.pause == 0 && !(code & 0x80);
        spin_unlock(&kbd_lock);
        if (press) {
            input_report_key(&ps2kbd_dev, KEY_PAUSE, 1);
            input_sync(&ps2kbd_dev);
            input_report_key(&ps2kbd_dev, KEY_PAUSE, 0);
            input_sync(&ps2kbd_dev);
        }
        return;
    }
    if (code == SC_PAUSE) {
        kbd.pause = 2;
        spin_unlock(&kbd_lock);
        return;
    }
    if (code == SC_EXTENDED) {
        kbd.extended = true;
        spin_unlock(&kbd_lock);
        return;
    }
    bool ext = kbd.extended;
    kbd.extended = false;
    spin_unlock(&kbd_lock);

    bool release = code & 0x80;
    code &= 0x7f;
    uint16_t key;
    if (ext) {
        /* 0xe0 0x2a and 0xe0 0x36 are the fake shifts around the
         * navigation keys and Print Screen. */
        key = extended_keys[code];
    } else {
        key = code <= KEY_F12 ? code : 0;
    }
    if (!key)
        return;
    input_report_key(&ps2kbd_dev, key, !release);
    input_sync(&ps2kbd_dev);
}

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

struct input_dev *ps2kbd_device(void)
{
    return &ps2kbd_dev;
}

void ps2kbd_init(void)
{
    input_dev_init(&ps2kbd_dev, "AT Translated Set 2 keyboard", BUS_I8042);
    for (unsigned code = KEY_ESC; code <= KEY_F12; code++)
        input_set_key_cap(&ps2kbd_dev, code);
    for (unsigned i = 0; i < ARRAY_SIZE(extended_keys); i++)
        if (extended_keys[i])
            input_set_key_cap(&ps2kbd_dev, extended_keys[i]);
    input_set_repeat(&ps2kbd_dev, PS2KBD_REPEAT_DELAY_MS, PS2KBD_REPEAT_PERIOD_MS);
    input_register_device(&ps2kbd_dev);
    /* Drain anything pending, then unmask the line. */
    while (inb(PS2_STATUS) & 0x01)
        (void)inb(PS2_DATA);
    irq_register(IRQ_KEYBOARD, kbd_irq, NULL);
    ioapic_route(GSI_KEYBOARD, IRQ_KEYBOARD, false);
    klog_info("ps/2 keyboard on irq %u", IRQ_KEYBOARD);
}
