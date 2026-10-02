#!/usr/bin/env python3
"""Compile the keyboard layouts of user/share/keymaps/src into MKM2 files
(L5, docs/design/keymaps.md).

  genkeymap.py              compiles every layout into user/share/keymaps
  genkeymap.py NAME OUT     compiles one layout

A layout source has these lines:
  name "Display name"
  include OTHER             starts from the first group of another layout
  switch alt_shift          Alt+Shift switches between two groups
  group 2                   the following keys belong to the second group
  key NAME PLAIN [SHIFT [ALTGR [ALTGR_SHIFT]]]

NAME is a key of kernel/include/minios/input.h without KEY_, in lower
case.  A value is one character, U+XXXX, "none", "space", or a dead key
(dead_grave, dead_acute, dead_circumflex, dead_tilde, dead_diaeresis,
dead_cedilla).  "\\" is a backslash and \\" a double quote.

MKM2 file: "MKM2", then the uint16 values entries (256), levels (4),
groups, switch (0 none, 1 Alt+Shift), compositions and flags (bit 0: the
layout uses AltGr), then the table of groups * entries * levels uint16
values (plain, Shift, AltGr, AltGr+Shift; 0 for none, a code point, a
symbol >= 0xe000, or 0xe100 + n for the dead key of the combining mark
U+0300 + n), then the compositions as uint16 triples (dead key, base,
result), sorted.  Keys without a character (Esc, the function keys, the
keypad, the cursor keys and the modifiers) are the same in every layout.
"""
import os
import re
import struct
import sys
import unicodedata

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(TOP, 'user', 'share', 'keymaps', 'src')
OUT = os.path.join(TOP, 'user', 'share', 'keymaps')

SYM = {
    "esc": 0xe001, "backspace": 0x0008, "tab": 0x0009, "enter": 0x000a,
    "up": 0xe010, "down": 0xe011, "left": 0xe012, "right": 0xe013, "home": 0xe014, "end": 0xe015,
    "pgup": 0xe016, "pgdn": 0xe017, "insert": 0xe018, "delete": 0xe019,
    "shift": 0xe030, "ctrl": 0xe031, "alt": 0xe032, "caps": 0xe033, "logo": 0xe034, "menu": 0xe035,
}
DEAD = {"dead_grave": 0x300, "dead_acute": 0x301, "dead_circumflex": 0x302, "dead_tilde": 0x303,
        "dead_diaeresis": 0x308, "dead_cedilla": 0x327}
DEAD_BASE = 0xe100


def key_codes():
    codes = {}
    with open(os.path.join(TOP, 'kernel', 'include', 'minios', 'input.h')) as f:
        for line in f:
            m = re.match(r'#define KEY_(\w+)\s+(\d+)', line)
            if m:
                codes[m.group(1).lower()] = int(m.group(2))
    return codes


def common_keys():
    """The keys without a character, the same in every layout."""
    t = {}
    t[1] = [SYM["esc"]] * 4
    t[14] = [SYM["backspace"]] * 4
    t[15] = [SYM["tab"]] * 4
    t[28] = [SYM["enter"]] * 4
    t[57] = [0x20] * 4                              # KEY_SPACE
    t[42] = t[54] = [SYM["shift"]] * 4
    t[29] = t[97] = [SYM["ctrl"]] * 4
    t[56] = t[100] = [SYM["alt"]] * 4
    t[125] = t[126] = [SYM["logo"]] * 4
    t[127] = [SYM["menu"]] * 4
    t[58] = [SYM["caps"]] * 4
    for i in range(10):
        t[59 + i] = [0xe021 + i] * 4                # KEY_F1..KEY_F10
    t[87] = [0xe02b] * 4
    t[88] = [0xe02c] * 4
    for code, ch in {71: "7", 72: "8", 73: "9", 74: "-", 75: "4", 76: "5", 77: "6", 78: "+",
                     79: "1", 80: "2", 81: "3", 82: "0", 83: ".", 98: "/", 55: "*"}.items():
        t[code] = [ord(ch)] * 4
    t[96] = [0x0a] * 4                              # KEY_KPENTER
    for code, name in {103: "up", 108: "down", 105: "left", 106: "right", 102: "home", 107: "end",
                       104: "pgup", 109: "pgdn", 110: "insert", 111: "delete"}.items():
        t[code] = [SYM[name]] * 4
    return t


def value(token):
    if token == 'none':
        return 0
    if token == 'space':
        return 0x20
    if token in DEAD:
        return DEAD_BASE + DEAD[token] - 0x300
    if token == '\\"':
        return ord('"')
    if token.startswith('U+'):
        return int(token[2:], 16)
    if len(token) != 1:
        raise ValueError(f'unknown value {token!r}')
    return ord(token)


def parse(name, codes, seen=()):
    """Return (display name, groups, switch) of a layout.  groups is a list
    of dictionaries from key codes to four values."""
    if name in seen:
        raise ValueError(f'{name} includes itself')
    display, groups, switch, group = name, [{}], 0, 0
    with open(os.path.join(SRC, name + '.kmap'), encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            words = line.split()
            try:
                if words[0] == 'name':
                    display = line.split('"')[1]
                elif words[0] == 'include':
                    _, other, _ = parse(words[1], codes, seen + (name,))
                    groups[0] = dict(other[0])
                elif words[0] == 'switch':
                    switch = {'alt_shift': 1}[words[1]]
                elif words[0] == 'group':
                    group = int(words[1]) - 1
                    while len(groups) <= group:
                        groups.append({})
                elif words[0] == 'key':
                    code = codes[words[1]]
                    vals = [value(w) for w in words[2:6]]
                    if len(vals) == 1:
                        vals.append(vals[0])
                    groups[group][code] = (vals + [0, 0])[:4]
                else:
                    raise ValueError(f'unknown line {words[0]!r}')
            except (ValueError, KeyError, IndexError) as e:
                raise SystemExit(f'{name}.kmap:{n}: {e}')
    return display, groups, switch


def compositions(groups):
    """Every dead key of the layout combined with the letters and the
    space: the precomposed letter of Unicode, or the spacing accent for the
    space."""
    out = {}
    dead = {v for g in groups for vals in g.values() for v in vals if DEAD_BASE <= v < DEAD_BASE + 0x70}
    for d in sorted(dead):
        mark = chr(0x300 + d - DEAD_BASE)
        spacing = {0x300: '`', 0x301: '´', 0x302: '^', 0x303: '~', 0x308: '¨', 0x327: '¸'}
        out[(d, 0x20)] = ord(spacing[ord(mark)])
        for base in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ':
            composed = unicodedata.normalize('NFC', base + mark)
            if len(composed) == 1:
                out[(d, ord(base))] = ord(composed)
    return out


def write(name, out_path, codes):
    display, groups, switch = parse(name, codes)
    common = common_keys()
    uses_altgr = any(v[2] or v[3] for g in groups for v in g.values())
    comp = compositions(groups)
    data = b'MKM2' + struct.pack('<6H', 256, 4, len(groups), switch, len(comp), 1 if uses_altgr else 0)
    for i, g in enumerate(groups):
        for code in range(256):
            vals = g.get(code) or (common.get(code) if i == 0 else None) or [0, 0, 0, 0]
            data += struct.pack('<4H', *vals)
    for (d, base), result in sorted(comp.items()):
        data += struct.pack('<3H', d, base, result)
    with open(out_path, 'wb') as f:
        f.write(data)
    print(f'genkeymap: {name} ({display}): {len(groups)} groups, {len(comp)} compositions -> {out_path}')


def main():
    codes = key_codes()
    if len(sys.argv) == 3:
        write(sys.argv[1], sys.argv[2], codes)
        return
    for f in sorted(os.listdir(SRC)):
        if f.endswith('.kmap'):
            name = f[:-5]
            write(name, os.path.join(OUT, name + '.mkm'), codes)


if __name__ == '__main__':
    main()
