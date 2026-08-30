#pragma once
/* Keymaps shared by the compositor (M25): key codes and modifier
 * levels to characters or symbols. Symbols are values >= 0xe000. */
#include <stdint.h>
#include <stddef.h>

enum keysym {
    KEY_ESC = 0xe001, KEY_UP = 0xe010, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE, KEY_F1 = 0xe021, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
    KEY_F10, KEY_F11, KEY_F12, KEY_SHIFT = 0xe030, KEY_CTRL, KEY_ALT, KEY_CAPS,
};

#define KEYMAP_MOD_SHIFT 1
#define KEYMAP_MOD_CTRL 2
#define KEYMAP_MOD_ALT 4

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
