/* The PS/2 keyboard: scancode set 1 bytes are translated to key codes and
 * reported to the input core, which retains the key state, repeats pressed keys
 * and feeds the console terminal. The 8042 controller that delivers the
 * bytes is platform code (arch/x86_64/i8042.c). */
#define KLOG_SUBSYS "ps2kbd"
#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>
#include <input/input.h>
#include <sync/spinlock.h>
#include <klog.h>

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

/* Key codes of the unprefixed scancodes above KEY_F12, the keys of the
 * Japanese keyboard. */
static uint16_t jis_key(uint8_t code)
{
    switch (code) {
    case 0x70: return KEY_KATAKANAHIRAGANA;
    case 0x73: return KEY_RO;
    case 0x79: return KEY_HENKAN;
    case 0x7b: return KEY_MUHENKAN;
    case 0x7d: return KEY_YEN;
    }
    return 0;
}

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
    /* LANG1 and LANG2 send 0xf2 and 0xf1 when pressed and nothing when
     * released. */
    if (!ext && (code == 0xf1 || code == 0xf2)) {
        uint16_t lang = code == 0xf2 ? KEY_HANGEUL : KEY_HANJA;
        input_report_key(&ps2kbd_dev, lang, 1);
        input_sync(&ps2kbd_dev);
        input_report_key(&ps2kbd_dev, lang, 0);
        input_sync(&ps2kbd_dev);
        return;
    }

    bool release = code & 0x80;
    code &= 0x7f;
    uint16_t key;
    if (ext) {
        /* 0xe0 0x2a and 0xe0 0x36 are the fake shifts around the
         * navigation keys and Print Screen. */
        key = extended_keys[code];
    } else {
        key = code <= KEY_F12 ? code : jis_key((uint8_t)code);
    }
    if (!key)
        return;
    input_report_key(&ps2kbd_dev, key, !release);
    input_sync(&ps2kbd_dev);
}

struct input_dev *ps2kbd_device(void)
{
    return &ps2kbd_dev;
}

void ps2kbd_register(void)
{
    input_dev_init(&ps2kbd_dev, "AT Translated Set 2 keyboard", BUS_I8042);
    for (unsigned code = KEY_ESC; code <= KEY_F12; code++)
        input_set_key_cap(&ps2kbd_dev, code);
    static const uint8_t jis[] = { 0x70, 0x73, 0x79, 0x7b, 0x7d };
    for (unsigned i = 0; i < ARRAY_SIZE(jis); i++)
        input_set_key_cap(&ps2kbd_dev, jis_key(jis[i]));
    input_set_key_cap(&ps2kbd_dev, KEY_HANGEUL);
    input_set_key_cap(&ps2kbd_dev, KEY_HANJA);
    for (unsigned i = 0; i < ARRAY_SIZE(extended_keys); i++)
        if (extended_keys[i])
            input_set_key_cap(&ps2kbd_dev, extended_keys[i]);
    input_set_repeat(&ps2kbd_dev, PS2KBD_REPEAT_DELAY_MS, PS2KBD_REPEAT_PERIOD_MS);
    input_register_device(&ps2kbd_dev);
}
