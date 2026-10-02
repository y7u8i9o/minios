#pragma once
/* Keymaps shared by the compositor (M25, L5): key codes (the Linux codes
 * of minios/input.h, KEY_*) and modifier levels to characters or symbols.
 * Symbols are values >= 0xe000.  A keymap has one or two groups of the
 * levels plain, Shift, AltGr and AltGr+Shift, dead keys and their
 * compositions (docs/design/keymaps.md). */
#include <stdint.h>
#include <stddef.h>
#include <minios/input.h>

enum keysym {
    KS_ESC = 0xe001, KS_UP = 0xe010, KS_DOWN, KS_LEFT, KS_RIGHT, KS_HOME, KS_END, KS_PGUP, KS_PGDN,
    KS_INSERT, KS_DELETE, KS_F1 = 0xe021, KS_F2, KS_F3, KS_F4, KS_F5, KS_F6, KS_F7, KS_F8, KS_F9,
    KS_F10, KS_F11, KS_F12, KS_SHIFT = 0xe030, KS_CTRL, KS_ALT, KS_CAPS, KS_LOGO, KS_MENU,
};

#define KEYMAP_MOD_SHIFT 1
#define KEYMAP_MOD_CTRL 2
#define KEYMAP_MOD_ALT 4
#define KEYMAP_MOD_LOGO 8
#define KEYMAP_MOD_ALTGR 16     /* the right Alt key of a layout that uses AltGr */
#define KEYMAP_MOD_CAPS 32      /* Caps Lock is locked */

/* A dead key is the symbol KS_DEAD + n for the combining mark U+0300 + n. */
#define KS_DEAD 0xe100
static inline int keysym_is_dead(int v) { return v >= KS_DEAD && v < KS_DEAD + 0x70; }

struct keymap {
    int entries, levels;
    uint16_t *table;            /* groups x entries x levels */
    void *map;
    size_t size;
    int groups;                 /* 1 or 2 */
    int switch_mode;            /* 1: Alt+Shift switches the group */
    int uses_altgr;             /* the layout has characters on the AltGr levels */
    int ncompose;
    uint16_t *compose;          /* ncompose triples (dead key, base, result), sorted */
};

/* Load from a descriptor and size (as sent by the compositor) or a path. */
struct keymap *keymap_from_fd(int fd, size_t size);
struct keymap *keymap_load(const char *path);
void keymap_free(struct keymap *k);
/* Character (or symbol) for a key code under modifiers in the first group;
 * 0 when none. */
int keymap_translate(const struct keymap *k, uint32_t key, int modifiers);
/* The same in group 0 or 1.  Ctrl and Alt combinations use the first
 * group, and shortcuts are therefore the same in every group. */
int keymap_translate_group(const struct keymap *k, uint32_t key, int modifiers, int group);
/* The character that a dead key and a following character compose, or 0. */
int keymap_compose(const struct keymap *k, int dead, int base);
static inline int keysym_is_symbol(int v) { return v >= 0xe000; }
