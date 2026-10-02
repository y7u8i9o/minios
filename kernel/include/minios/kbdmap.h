#pragma once
/* The keymap of the console keyboard (L5, docs/design/keymaps.md).
 * loadkeys converts an MKM2 file into this structure and sets it with
 * ioctl(fd, KBD_SET_KEYMAP, &map) on /dev/console.  The values have the
 * meaning of the MKM2 tables: a code point, 0 for none, a symbol from
 * 0xe000, or 0xe100 + n for the dead key of the combining mark U+0300 + n. */
#include <stdint.h>

#define KBD_SET_KEYMAP 0x4b01
#define KBD_KEYS 128
#define KBD_COMPOSE_MAX 128

struct kbd_keymap {
    uint16_t groups;                        /* 1 or 2 */
    uint16_t switch_mode;                   /* 1: Alt+Shift switches the group */
    uint16_t uses_altgr;                    /* the right Alt key is AltGr */
    uint16_t ncompose;
    uint16_t table[2][KBD_KEYS][4];         /* plain, Shift, AltGr, AltGr+Shift */
    uint16_t compose[KBD_COMPOSE_MAX][3];   /* dead key, base, result; sorted */
};
