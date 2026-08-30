#!/usr/bin/env python3
"""Write a keymap file for the compositor and its clients.

  genkeymap.py <out.mkm>

Layout: "MKM1", uint16 entries (256), uint16 levels (4), then for each
key code (PS/2 set 1 scancode; 0x80 + code for the 0xe0 prefixed ones)
four uint16 values for the plain, Shift, Ctrl and Alt levels: the
character produced, 0 for none, or a symbol >= 0xe000 for keys without
a character (see libgui/include/gui/keymap.h for the symbol numbers).
"""
import struct
import sys

SYM = {
    "esc": 0xe001, "backspace": 0x0008, "tab": 0x0009, "enter": 0x000a,
    "up": 0xe010, "down": 0xe011, "left": 0xe012, "right": 0xe013, "home": 0xe014, "end": 0xe015,
    "pgup": 0xe016, "pgdn": 0xe017, "insert": 0xe018, "delete": 0xe019,
    "f1": 0xe021, "f2": 0xe022, "f3": 0xe023, "f4": 0xe024, "f5": 0xe025, "f6": 0xe026,
    "f7": 0xe027, "f8": 0xe028, "f9": 0xe029, "f10": 0xe02a, "f11": 0xe02b, "f12": 0xe02c,
    "shift": 0xe030, "ctrl": 0xe031, "alt": 0xe032, "caps": 0xe033,
}

PLAIN = "\x00\x001234567890-=\x08\tqwertyuiop[]\n\x00asdfghjkl;'`\x00\\zxcvbnm,./\x00*\x00 "
SHIFT = "\x00\x00!@#$%^&*()_+\x08\tQWERTYUIOP{}\n\x00ASDFGHJKL:\"~\x00|ZXCVBNM<>?\x00*\x00 "

table = [[0, 0, 0, 0] for _ in range(256)]
for code in range(2, len(PLAIN)):
    p, s = ord(PLAIN[code]), ord(SHIFT[code])
    ctrl = (p - ord("a") + 1) if "a" <= chr(p) <= "z" else p
    table[code] = [p, s, ctrl, p]
table[0x01] = [SYM["esc"]] * 4
table[0x2a] = table[0x36] = [SYM["shift"]] * 4
table[0x1d] = [SYM["ctrl"]] * 4
table[0x38] = [SYM["alt"]] * 4
table[0x3a] = [SYM["caps"]] * 4
for i in range(10):
    table[0x3b + i] = [SYM["f%d" % (i + 1)]] * 4
table[0x57] = [SYM["f11"]] * 4
table[0x58] = [SYM["f12"]] * 4
ext = {0x48: "up", 0x50: "down", 0x4b: "left", 0x4d: "right", 0x47: "home", 0x4f: "end",
       0x49: "pgup", 0x51: "pgdn", 0x52: "insert", 0x53: "delete", 0x1c: "enter"}
for code, name in ext.items():
    v = SYM[name] if name != "enter" else 0x0a
    table[0x80 | code] = [v] * 4

with open(sys.argv[1], "wb") as f:
    f.write(b"MKM1" + struct.pack("<HH", 256, 4))
    for row in table:
        f.write(struct.pack("<4H", *row))
