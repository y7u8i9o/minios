#pragma once
/* Keymaps shared by the compositor (M25): key codes (the Linux codes of
 * minios/input.h, KEY_*) and modifier levels to characters or symbols.
 * Symbols are values >= 0xe000. */
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

struct keymap {
    int entries, levels;
    uint16_t *table;            /* entries x levels */
    void *map;
    size_t size;
};

/* Load from a descriptor and size (as sent by the compositor) or a path. */
struct keymap *keymap_from_fd(int fd, size_t size);
struct keymap *keymap_load(const char *path);
void keymap_free(struct keymap *k);
/* Character (or symbol) for a key code under modifiers; 0 when none. */
int keymap_translate(const struct keymap *k, uint32_t key, int modifiers);
static inline int keysym_is_symbol(int v) { return v >= 0xe000; }
