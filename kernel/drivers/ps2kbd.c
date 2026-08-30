#define KLOG_SUBSYS "ps2kbd"
#include <drivers/ps2kbd.h>
#include <arch/apic.h>
#include <arch/irq.h>
#include <arch/io.h>
#include <sync/spinlock.h>
#include <console.h>
#include <klog.h>
#include <sched/wait.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <ipc/mqueue.h>
#include <errno.h>
#include <minios/abi.h>
#include <drivers/ps2mouse.h>
#include <drivers/tty.h>
#include <lib/string.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64

/* Scancode set 1, unshifted and shifted, for codes 0 to 0x57. */
static const char keymap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#define SC_LSHIFT   0x2a
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1d
#define SC_ALT      0x38
#define SC_CAPS     0x3a
#define SC_EXTENDED 0xe0

/* Modifier state. Protected by kbd_lock, taken in the interrupt handler. */
static DEFINE_SPINLOCK(kbd_lock);
static struct {
    bool shift, ctrl, alt, caps, extended;
} kbd;

static void line_input(char c)
{
    tty_input_char(&console_tty, c);
}

static void raw_push(const char *s)
{
    tty_input_raw(&console_tty, s, strlen(s));
}

static void feed_locked(uint8_t code)
{
    if (code == SC_EXTENDED) {
        kbd.extended = true;
        return;
    }
    bool release = code & 0x80;
    code &= 0x7f;
    bool ext = kbd.extended;
    kbd.extended = false;

    switch (code) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        kbd.shift = !release;
        return;
    case SC_CTRL:
        kbd.ctrl = !release;
        return;
    case SC_ALT:
        kbd.alt = !release;
        return;
    case SC_CAPS:
        if (!release)
            kbd.caps = !kbd.caps;
        return;
    }
    if (release)
        return;
    if (ext) {
        /* Cursor and editing keys become VT100 sequences in raw mode and
         * are ignored in canonical mode, except keypad enter. */
        const char *seq = NULL;
        switch (code) {
        case 0x48: seq = "\033[A"; break;
        case 0x50: seq = "\033[B"; break;
        case 0x4d: seq = "\033[C"; break;
        case 0x4b: seq = "\033[D"; break;
        case 0x47: seq = "\033[H"; break;
        case 0x4f: seq = "\033[F"; break;
        case 0x49: seq = "\033[5~"; break;
        case 0x51: seq = "\033[6~"; break;
        case 0x53: seq = "\033[3~"; break;
        case 0x1c: line_input('\n'); return;
        }
        if (seq && !(tty_get_lflag(&console_tty) & ICANON))
            raw_push(seq);
        return;
    }

    char c = kbd.shift ? keymap_shift[code] : keymap[code];
    if (!c)
        return;
    if (kbd.caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (kbd.ctrl) {
        if (c >= 'a' && c <= 'z')
            c = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 1;
        else if (c == '[')
            c = 27;
        else
            return;
    }
    if (c == 27 && !kbd.ctrl && !(tty_get_lflag(&console_tty) & ICANON)) {
        /* The Escape key itself. */
        raw_push("\033");
        return;
    }
    line_input(c);
}

void ps2kbd_feed_scancode(uint8_t code)
{
    if (tty_get_lflag(&console_tty) & KBD_SCANCODES) {
        char c = (char)code;
        tty_input_raw(&console_tty, &c, 1);
        return;
    }
    spin_lock(&kbd_lock);
    feed_locked(code);
    spin_unlock(&kbd_lock);
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

int ps2kbd_getc(void)
{
    return tty_getc(&console_tty);
}

size_t ps2kbd_available(void)
{
    return tty_available(&console_tty);
}

void ps2kbd_init(void)
{
    console_tty_init();
    /* Drain anything pending, then unmask the line. */
    while (inb(PS2_STATUS) & 0x01)
        (void)inb(PS2_DATA);
    irq_register(IRQ_KEYBOARD, kbd_irq, NULL);
    ioapic_route(GSI_KEYBOARD, IRQ_KEYBOARD, false);
    klog_info("ps/2 keyboard on irq %u", IRQ_KEYBOARD);
}

long ps2kbd_read(char *buf, size_t n)
{
    return tty_read(&console_tty, buf, n);
}

void ps2kbd_set_fg_pgid(int pgid)
{
    tty_set_fg_pgid(&console_tty, pgid);
}

int ps2kbd_get_fg_pgid(void)
{
    return tty_get_fg_pgid(&console_tty);
}

void ps2kbd_start_ttyd(void)
{
    tty_start_daemon();
}

uint32_t ps2kbd_get_lflag(void)
{
    return tty_get_lflag(&console_tty);
}

void ps2kbd_set_lflag(uint32_t lflag)
{
    tty_set_lflag(&console_tty, lflag);
}

int ps2kbd_poll(void)
{
    return tty_poll(&console_tty);
}
