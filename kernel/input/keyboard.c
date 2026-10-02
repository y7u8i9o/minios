/* The console keyboard: keys of every keyboard that no reader has
 * grabbed, translated with the modifier state to characters for the
 * console terminal. Key repeats from the core count as presses. */
#define KLOG_SUBSYS "kbd"
#include <input/input.h>
#include <drivers/tty.h>
#include <sync/spinlock.h>
#include <lib/string.h>
#include <minios/abi.h>
#include <minios/kbdmap.h>
#include <errno.h>

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

/* Modifier and lock state over every keyboard, and the layout that
 * loadkeys set (L5). Protected by kbd_lock, taken after input_dev.lock has
 * been released and before console_tty.lock.  Without a layout the tables
 * above are used. */
static DEFINE_SPINLOCK(kbd_lock);
static struct {
    uint8_t shift, ctrl, alt;       /* bit 0 left key, bit 1 right key */
    bool altgr;                     /* the right Alt key of a layout that uses AltGr */
    bool caps;
    int group;
    uint16_t dead;                  /* a dead key that waits for its base, or 0 */
} kbd;
static struct kbd_keymap layout;
static bool layout_set;

int input_console_set_keymap(const struct kbd_keymap *map)
{
    if (map->groups < 1 || map->groups > 2 || map->ncompose > KBD_COMPOSE_MAX)
        return -EINVAL;
    spin_lock(&kbd_lock);
    layout = *map;
    layout_set = true;
    kbd.group = 0;
    kbd.dead = 0;
    spin_unlock(&kbd_lock);
    return 0;
}

static bool is_letter(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c < 0x2000 && c != 0xd7 && c != 0xf7);
}

/* layout_char returns the value of a key in the layout.  The caller has
 * acquired kbd_lock. */
static uint32_t layout_char(uint16_t code, bool shift, bool altgr, bool caps, int group)
{
    if (code >= KBD_KEYS)
        return 0;
    uint16_t plain = layout.table[group][code][0] ? layout.table[group][code][0] : layout.table[0][code][0];
    if (caps && !altgr && is_letter(plain))
        shift = !shift;
    int level = (altgr ? 2 : 0) + (shift ? 1 : 0);
    uint32_t v = layout.table[group][code][level];
    if (!v && group)
        v = layout.table[0][code][level];
    if (!v && altgr)
        v = layout.table[group][code][shift] ? layout.table[group][code][shift] : layout.table[0][code][shift];
    return v;
}

/* layout_compose returns the composition of a dead key and a base. */
static uint32_t layout_compose(uint16_t dead, uint32_t base)
{
    for (unsigned i = 0; i < layout.ncompose; i++)
        if (layout.compose[i][0] == dead && layout.compose[i][1] == base)
            return layout.compose[i][2];
    return 0;
}

/* put_utf8 queues a code point for the console terminal. */
static void put_utf8(uint32_t cp)
{
    char b[4];
    int n = 0;
    if (cp < 0x80) {
        b[n++] = (char)cp;
    } else if (cp < 0x800) {
        b[n++] = (char)(0xc0 | cp >> 6);
        b[n++] = (char)(0x80 | (cp & 0x3f));
    } else {
        b[n++] = (char)(0xe0 | cp >> 12);
        b[n++] = (char)(0x80 | (cp >> 6 & 0x3f));
        b[n++] = (char)(0x80 | (cp & 0x3f));
    }
    for (int i = 0; i < n; i++)
        tty_input_char(&console_tty, b[i]);
}

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
    /* Alt+Shift switches the group of a layout with two groups when the
     * second of the two keys goes down. */
    bool two_groups = layout_set && layout.groups == 2 && layout.switch_mode == 1;
    switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        if (value == 1 && two_groups && kbd.alt && !kbd.shift)
            kbd.group = !kbd.group;
        set_modifier(&kbd.shift, code == KEY_RIGHTSHIFT, value);
        goto out;
    case KEY_LEFTCTRL:   set_modifier(&kbd.ctrl, 0, value); goto out;
    case KEY_RIGHTCTRL:  set_modifier(&kbd.ctrl, 1, value); goto out;
    case KEY_RIGHTALT:
        if (layout_set && layout.uses_altgr) {
            if (value != 2)
                kbd.altgr = value != 0;
            goto out;
        }
        /* fall through */
    case KEY_LEFTALT:
        if (value == 1 && two_groups && kbd.shift && !kbd.alt)
            kbd.group = !kbd.group;
        set_modifier(&kbd.alt, code == KEY_RIGHTALT, value);
        goto out;
    case KEY_CAPSLOCK:
        if (value == 1)
            kbd.caps = !kbd.caps;
        goto out;
    }
    if (value == 0)
        goto out;
    bool shift = kbd.shift != 0, ctrl = kbd.ctrl != 0, alt = kbd.alt != 0, caps = kbd.caps;
    /* With a layout, the key gives a code point.  A dead key waits for the
     * next character, and the two compose one character, or the accent
     * and the character follow each other.  Ctrl and Alt combinations use
     * the first group. */
    uint32_t cp = 0, spacing = 0;
    if (layout_set) {
        cp = ctrl || alt ? (code < KBD_KEYS ? layout.table[0][code][0] : 0)
                         : layout_char(code, shift, kbd.altgr, caps, kbd.group);
        if (cp >= 0xe100 && cp < 0xe170) {
            if (kbd.dead)
                spacing = layout_compose(kbd.dead, ' ');
            kbd.dead = (uint16_t)cp;
            spin_unlock(&kbd_lock);
            if (spacing)
                put_utf8(spacing);
            return;
        }
        if (kbd.dead && cp >= 0x20 && cp < 0xe000) {
            uint32_t composed = layout_compose(kbd.dead, cp);
            if (composed)
                cp = composed;
            else
                spacing = layout_compose(kbd.dead, ' ');
            kbd.dead = 0;
        }
        if (cp >= 0xe000)
            cp = 0;
    }
    spin_unlock(&kbd_lock);

    bool canon = (tty_get_lflag(&console_tty) & ICANON) != 0;
    if (spacing)
        put_utf8(spacing);
    if (cp >= 0x80 && !ctrl) {
        if (alt && !canon)
            tty_input_raw(&console_tty, "\033", 1);
        put_utf8(cp);
        return;
    }
    char c = 0;
    if (layout_set)
        c = code == KEY_ESC ? 27 : (char)cp;
    else if (code < ARRAY_SIZE(plain))
        c = shift ? shifted[code] : plain[code];
    if (!c && code == KEY_KPENTER)
        c = '\n';
    else if (!c && code == KEY_KPSLASH)
        c = '/';
    if (!c) {
        /* Cursor, editing and function keys become escape sequences in
         * raw mode and are ignored by the line editor. */
        const char *seq = sequence(code);
        if (seq && !canon)
            tty_input_raw(&console_tty, seq, strlen(seq));
        return;
    }
    if (!layout_set && caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
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
