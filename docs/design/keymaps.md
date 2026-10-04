# Keyboard layouts

The console and the compositor translate key codes with the same keyboard
layouts. A layout has one or two groups of four levels, dead keys with a
table of compositions, and an optional AltGr level. The layouts are `us`,
`fr` (AZERTY), `es`, `ru` (English and Russian groups) and `jp` (JIS 106).

## Sources and files

A layout is written as a text file `user/share/keymaps/src/NAME.kmap`:

| Line | Meaning |
|---|---|
| `name "Display name"` | the name of the layout in its own language |
| `include OTHER` | the first group starts as the first group of another layout |
| `switch alt_shift` | Alt+Shift switches between the two groups |
| `group 2` | the following key lines belong to the second group |
| `key NAME PLAIN [SHIFT [ALTGR [ALTGR_SHIFT]]]` | the characters of a key |

`NAME` is a key of `kernel/include/minios/input.h` without `KEY_`, in lower
case, such as `q`, `leftbrace` or `102nd`. A value is one character,
`U+XXXX`, `none`, `space` or a dead key (`dead_grave`, `dead_acute`,
`dead_circumflex`, `dead_tilde`, `dead_diaeresis`, `dead_cedilla`). A key
line with one value uses it for Shift too. The keys without a character
(Escape, Tab, Enter, Backspace, the function keys, the keypad, the cursor
keys and the modifiers) are the same in every layout.

`tools/genkeymap/genkeymap.py` compiles every source into
`user/share/keymaps/NAME.mkm`, which is checked in. An MKM2 file begins
with `MKM2` and the 16-bit values entries (256), levels (4), groups,
switch mode, number of compositions and flags (bit 0: the layout has
characters on the AltGr levels). The table follows with groups × 256 × 4
16-bit values for plain, Shift, AltGr and AltGr+Shift: 0 for none, a code
point, a symbol from 0xe000 (`gui/keymap.h`), or `KS_DEAD + n`, 0xe100 + n,
for the dead key of the combining mark U+0300 + n. The compositions follow
as sorted triples of dead key, base and result. The generator composes
every dead key of a layout with the letters through Unicode normalization,
and with the space to the spacing accent. A value 0 in the second group
takes the value of the first group.

The files of the old format MKM1 (plain, Shift, Ctrl and Alt levels) are
still read, as one group without AltGr.

## Translation

`keymap_translate_group` in `lib/libgui/src/keymap.c` translates a key code
with the modifiers and a group. Ctrl and Alt combinations use the plain
level of the first group. Shortcuts such as Ctrl+C are therefore the same
in every group, and Ctrl with a letter gives the control character.
Otherwise the level is AltGr plus Shift. Caps Lock selects the Shift level
of a letter. An empty value of the second group takes the first group, and
an empty AltGr level takes the plain or Shift level. `keymap_compose`
returns the composition of a dead key and a character.

## Compositor

X12 loads `/usr/share/keymaps/NAME.mkm` for the `keymap` setting
(`seat_load_keymap`), copies the file into a memfd of its size and sends it
to every bound keyboard, also after a reload. The right Alt key is AltGr
(`KEYMAP_MOD_ALTGR`) when the layout uses AltGr, otherwise Alt. Caps Lock
toggles a locked state, and Alt+Shift switches the group of a layout with
two groups when the second of the two keys goes down. The `modifiers`
event carries the pressed modifiers, Caps Lock in its `locked` argument and
the group in its `group` argument. A new keymap starts in the first group.

For a client with text input enabled, the compositor commits the
characters (`text.c`). A dead key shows its accent as the preedit and waits
for the next character. The two commit their composition, or the accent
and the character when the layout has no composition for them. A second
dead key commits the accent of the first, and Escape or Backspace drops
the accent.

libgui (`client.c`) takes the group and Caps Lock from the `modifiers`
event and translates its key events with them. In a window without text
input, such as the terminal, libgui composes dead keys itself. Widgets
receive only the modifiers Shift, Ctrl, Alt and Logo. Accelerators
therefore work with Caps Lock locked. The launcher search accepts every printable
character and compares titles without regard to case through `towlower`.

## Console

The console keyboard (`kernel/input/keyboard.c`) uses its fixed US tables
until `loadkeys` sets a layout. `loadkeys NAME` converts an MKM file into
`struct kbd_keymap` of `<minios/kbdmap.h>` (the first 128 key codes and up
to 128 compositions) and sets it with the `ioctl` `KBD_SET_KEYMAP` on
`/dev/console`. `loadkeys -c` takes the name from the `keymap` setting of
the desktop configuration, and init runs it as the task `keymap` after
`fsinit`. With a layout, the console follows the same rules as the
compositor for the levels, Caps Lock, the group switch and dead keys, and
it sends characters above U+007F to the terminal as UTF-8. Escape gives
the byte 27.

The console output (`kernel/drivers/fbcon.c`) decodes UTF-8. The console
font has glyphs for the code points below 256, and other characters are
drawn as a question mark.

The PS/2 decoder translates the scancodes of the Japanese keyboard, 0x70,
0x73, 0x79, 0x7b and 0x7d, to `KEY_KATAKANAHIRAGANA`, `KEY_RO`,
`KEY_HENKAN`, `KEY_MUHENKAN` and `KEY_YEN`.

## Tests

The boot test `keymap` runs `loadkeys` for `fr`, `es`, `ru` and `jp` and
feeds scancodes to the console: AZERTY letters, a dead circumflex, AltGr,
a dead acute and a dead diaeresis, a dead acute before a space, the
Russian group after Alt+Shift and back, and the yen and ro keys. It then
sets `keymap=fr` in the configuration, has X12 reload it, and types into
gedit with a dead key and AltGr, then reloads `ru` and types Russian
letters after Alt+Shift. gedit must save the five characters. `comp_seat`
checks the size of the MKM2 file of `us`.
