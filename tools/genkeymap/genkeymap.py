#!/usr/bin/env python3
"""Write a keymap file for the compositor and its clients.

  genkeymap.py <out.mkm>

Layout: "MKM1", uint16 entries (256), uint16 levels (4), then for each
key code (the Linux key codes of kernel/include/minios/input.h) four
uint16 values for the plain, Shift, Ctrl and Alt levels: the character
produced, 0 for none, or a symbol >= 0xe000 for keys without a
character (see libgui/include/gui/keymap.h for the symbol numbers).
"""
import struct
import sys

SYM = {
    "esc": 0xe001, "backspace": 0x0008, "tab": 0x0009, "enter": 0x000a,
    "up": 0xe010, "down": 0xe011, "left": 0xe012, "right": 0xe013, "home": 0xe014, "end": 0xe015,
    "pgup": 0xe016, "pgdn": 0xe017, "insert": 0xe018, "delete": 0xe019,
    "f1": 0xe021, "f2": 0xe022, "f3": 0xe023, "f4": 0xe024, "f5": 0xe025, "f6": 0xe026,
    "f7": 0xe027, "f8": 0xe028, "f9": 0xe029, "f10": 0xe02a, "f11": 0xe02b, "f12": 0xe02c,
    "shift": 0xe030, "ctrl": 0xe031, "alt": 0xe032, "caps": 0xe033, "logo": 0xe034, "menu": 0xe035,
}

# Key codes 1..88 equal the PS/2 set 1 make codes; these two strings are
# indexed by key code.
PLAIN = "\x00\x001234567890-=\x08\tqwertyuiop[]\n\x00asdfghjkl;'`\x00\\zxcvbnm,./\x00*\x00 "
SHIFT = "\x00\x00!@#$%^&*()_+\x08\tQWERTYUIOP{}\n\x00ASDFGHJKL:\"~\x00|ZXCVBNM<>?\x00*\x00 "

table = [[0, 0, 0, 0] for _ in range(256)]
for code in range(2, len(PLAIN)):
    p, s = ord(PLAIN[code]), ord(SHIFT[code])
    ctrl = (p - ord("a") + 1) if "a" <= chr(p) <= "z" else p
    table[code] = [p, s, ctrl, p]
table[1] = [SYM["esc"]] * 4                     # KEY_ESC
table[42] = table[54] = [SYM["shift"]] * 4      # KEY_LEFTSHIFT, KEY_RIGHTSHIFT
table[29] = table[97] = [SYM["ctrl"]] * 4       # KEY_LEFTCTRL, KEY_RIGHTCTRL
table[56] = table[100] = [SYM["alt"]] * 4       # KEY_LEFTALT, KEY_RIGHTALT
table[125] = table[126] = [SYM["logo"]] * 4     # KEY_LEFTMETA, KEY_RIGHTMETA
table[127] = [SYM["menu"]] * 4                  # KEY_COMPOSE
table[58] = [SYM["caps"]] * 4                   # KEY_CAPSLOCK
for i in range(10):
    table[59 + i] = [SYM["f%d" % (i + 1)]] * 4  # KEY_F1..KEY_F10
table[87] = [SYM["f11"]] * 4
table[88] = [SYM["f12"]] * 4
# The keypad: digits and operators.
for code, ch in {71: "7", 72: "8", 73: "9", 74: "-", 75: "4", 76: "5", 77: "6", 78: "+",
                 79: "1", 80: "2", 81: "3", 82: "0", 83: ".", 98: "/", 55: "*"}.items():
    table[code] = [ord(ch)] * 4
table[96] = [0x0a] * 4                          # KEY_KPENTER
ext = {103: "up", 108: "down", 105: "left", 106: "right", 102: "home", 107: "end",
       104: "pgup", 109: "pgdn", 110: "insert", 111: "delete"}
for code, name in ext.items():
    table[code] = [SYM[name]] * 4

with open(sys.argv[1], "wb") as f:
    f.write(b"MKM1" + struct.pack("<HH", 256, 4))
    for row in table:
        f.write(struct.pack("<4H", *row))
