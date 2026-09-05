/* The console keyboard: keys of every keyboard that no reader has
 * grabbed, translated with the modifier state to characters for the
 * console terminal. Key repeats from the core count as presses. */
#define KLOG_SUBSYS "kbd"
#include <input/input.h>
#include <drivers/tty.h>
#include <sync/spinlock.h>
#include <lib/string.h>
#include <minios/abi.h>

/* Characters of the key codes 0..88 (the main block), plain and shifted. */
static const char plain[89] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, '\\', 0, 0,
};
static const char shifted[89] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, '|', 0, 0,
};

/* Modifier and lock state over every keyboard. Protected by kbd_lock,
 * taken after input_dev.lock has been released and before
 * console_tty.lock. */
static DEFINE_SPINLOCK(kbd_lock);
static struct {
    uint8_t shift, ctrl, alt;       /* bit 0 left key, bit 1 right key */
    bool caps;
} kbd;

/* Escape sequence of a key without a character, or NULL. */
static const char *sequence(uint16_t code)
{
    switch (code) {
    case KEY_UP:       return "\033[A";
    case KEY_DOWN:     return "\033[B";
    case KEY_RIGHT:    return "\033[C";
    case KEY_LEFT:     return "\033[D";
    case KEY_HOME:     return "\033[H";
    case KEY_END:      return "\033[F";
    case KEY_INSERT:   return "\033[2~";
    case KEY_DELETE:   return "\033[3~";
    case KEY_PAGEUP:   return "\033[5~";
    case KEY_PAGEDOWN: return "\033[6~";
    case KEY_F1:       return "\033OP";
    case KEY_F2:       return "\033OQ";
    case KEY_F3:       return "\033OR";
    case KEY_F4:       return "\033OS";
    case KEY_F5:       return "\033[15~";
    case KEY_F6:       return "\033[17~";
    case KEY_F7:       return "\033[18~";
    case KEY_F8:       return "\033[19~";
    case KEY_F9:       return "\033[20~";
    case KEY_F10:      return "\033[21~";
    case KEY_F11:      return "\033[23~";
    case KEY_F12:      return "\033[24~";
    }
    return NULL;
}

static void set_modifier(uint8_t *mask, int side, int value)
{
    if (value == 2)
        return;
    if (value)
        *mask |= (uint8_t)(1u << side);
    else
        *mask &= (uint8_t)~(1u << side);
}

void input_console_key(uint16_t code, int value)
{
    spin_lock(&kbd_lock);
    switch (code) {
    case KEY_LEFTSHIFT:  set_modifier(&kbd.shift, 0, value); goto out;
    case KEY_RIGHTSHIFT: set_modifier(&kbd.shift, 1, value); goto out;
    case KEY_LEFTCTRL:   set_modifier(&kbd.ctrl, 0, value); goto out;
    case KEY_RIGHTCTRL:  set_modifier(&kbd.ctrl, 1, value); goto out;
    case KEY_LEFTALT:    set_modifier(&kbd.alt, 0, value); goto out;
    case KEY_RIGHTALT:   set_modifier(&kbd.alt, 1, value); goto out;
    case KEY_CAPSLOCK:
        if (value == 1)
            kbd.caps = !kbd.caps;
        goto out;
    }
    if (value == 0)
        goto out;
    bool shift = kbd.shift != 0, ctrl = kbd.ctrl != 0, alt = kbd.alt != 0, caps = kbd.caps;
    spin_unlock(&kbd_lock);

    bool canon = (tty_get_lflag(&console_tty) & ICANON) != 0;
    char c = 0;
    if (code < ARRAY_SIZE(plain))
        c = shift ? shifted[code] : plain[code];
    else if (code == KEY_KPENTER)
        c = '\n';
    else if (code == KEY_KPSLASH)
        c = '/';
    if (!c) {
        /* Cursor, editing and function keys become escape sequences in
         * raw mode and are ignored by the line editor. */
        const char *seq = sequence(code);
        if (seq && !canon)
            tty_input_raw(&console_tty, seq, strlen(seq));
        return;
    }
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (ctrl) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        else if (c == '[')
            c = 27;
        else if (c == ' ')
            c = 0;
        else
            return;
    }
    if (!canon) {
        /* Escape itself, and Alt as an escape prefix, reach raw readers. */
        if (c == 27 && !ctrl) {
            tty_input_raw(&console_tty, "\033", 1);
            return;
        }
        if (alt)
            tty_input_raw(&console_tty, "\033", 1);
    }
    tty_input_char(&console_tty, c);
    return;
out:
    spin_unlock(&kbd_lock);
}
