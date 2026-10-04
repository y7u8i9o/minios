/* Keymap files (M25, L5, docs/design/keymaps.md).  MKM2 files have one or
 * two groups of four levels, the composition table of their dead keys and
 * a flag for the AltGr levels.  MKM1 files, which have the levels plain,
 * Shift, Ctrl and Alt, are read as one group without AltGr. */
#include <gui/keymap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define KEYMAP_FILE_MAX 65536

static unsigned u16(const uint8_t *p)
{
    return (unsigned)(p[0] | p[1] << 8);
}

static struct keymap *parse(const void *data, size_t size)
{
    const uint8_t *p = data;
    int v2 = size >= 16 && memcmp(p, "MKM2", 4) == 0;
    if (!v2 && (size < 8 || memcmp(p, "MKM1", 4) != 0))
        return NULL;
    int entries = (int)u16(p + 4), levels = (int)u16(p + 6);
    int groups = v2 ? (int)u16(p + 8) : 1, ncompose = v2 ? (int)u16(p + 12) : 0;
    size_t header = v2 ? 16 : 8;
    size_t table_size = (size_t)groups * entries * levels * 2;
    if (entries <= 0 || levels < 2 || groups < 1 || groups > 2 || size < header + table_size + (size_t)ncompose * 6)
        return NULL;
    struct keymap *k = calloc(1, sizeof *k);
    if (!k)
        return NULL;
    k->entries = entries;
    k->levels = 4;
    k->groups = groups;
    k->switch_mode = v2 ? (int)u16(p + 10) : 0;
    k->uses_altgr = v2 ? (int)(u16(p + 14) & 1) : 0;
    k->table = calloc((size_t)groups * entries * 4, sizeof *k->table);
    k->compose = calloc((size_t)ncompose * 3 + 1, sizeof *k->compose);
    if (!k->table || !k->compose) {
        keymap_free(k);
        return NULL;
    }
    const uint8_t *t = p + header;
    for (int g = 0; g < groups; g++)
        for (int e = 0; e < entries; e++)
            for (int l = 0; l < levels && l < 4; l++) {
                /* The Ctrl and Alt levels of MKM1 are computed by
                 * keymap_translate_group and are not stored. */
                if (!v2 && l >= 2)
                    continue;
                k->table[((size_t)g * entries + e) * 4 + l] = (uint16_t)u16(t + (((size_t)g * entries + e) * levels + l) * 2);
            }
    const uint8_t *c = t + table_size;
    for (int i = 0; i < ncompose * 3; i++)
        k->compose[i] = (uint16_t)u16(c + i * 2);
    k->ncompose = ncompose;
    k->size = size;
    return k;
}

struct keymap *keymap_from_fd(int fd, size_t size)
{
    size_t len = (size + 4095) & ~(size_t)4095;
    void *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        return NULL;
    struct keymap *k = parse(map, size);
    munmap(map, len);
    return k;
}

struct keymap *keymap_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    uint8_t *buf = malloc(KEYMAP_FILE_MAX);
    size_t n = buf ? fread(buf, 1, KEYMAP_FILE_MAX, f) : 0;
    fclose(f);
    struct keymap *k = buf ? parse(buf, n) : NULL;
    free(buf);
    return k;
}

void keymap_free(struct keymap *k)
{
    if (k) {
        free(k->table);
        free(k->compose);
        free(k);
    }
}

static int entry(const struct keymap *k, int group, uint32_t key, int level)
{
    return k->table[((size_t)group * k->entries + key) * 4 + level];
}

static int is_letter(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c < 0x2000 && c != 0xd7 && c != 0xf7);
}

int keymap_translate_group(const struct keymap *k, uint32_t key, int modifiers, int group)
{
    if (!k || key >= (uint32_t)k->entries)
        return 0;
    if (group < 0 || group >= k->groups)
        group = 0;
    if (modifiers & (KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT)) {
        int c = entry(k, 0, key, 0);
        if ((modifiers & KEYMAP_MOD_CTRL) && c >= 'a' && c <= 'z')
            return c - 'a' + 1;
        return c;
    }
    int shift = (modifiers & KEYMAP_MOD_SHIFT) != 0, altgr = (modifiers & KEYMAP_MOD_ALTGR) != 0;
    /* Caps Lock selects the Shift level of a letter. */
    if ((modifiers & KEYMAP_MOD_CAPS) && !altgr) {
        int plain = entry(k, group, key, 0);
        if (!plain && group)
            plain = entry(k, 0, key, 0);
        if (is_letter(plain))
            shift = !shift;
    }
    int level = (altgr ? 2 : 0) + shift;
    int v = entry(k, group, key, level);
    if (!v && group)
        v = entry(k, 0, key, level);
    if (!v && altgr)
        v = entry(k, group, key, shift) ? entry(k, group, key, shift) : entry(k, 0, key, shift);
    return v;
}

int keymap_translate(const struct keymap *k, uint32_t key, int modifiers)
{
    return keymap_translate_group(k, key, modifiers, 0);
}

int keymap_compose(const struct keymap *k, int dead, int base)
{
    if (!k)
        return 0;
    int lo = 0, hi = k->ncompose;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        const uint16_t *c = k->compose + mid * 3;
        int r = c[0] != dead ? (int)c[0] - dead : (int)c[1] - base;
        if (r == 0)
            return c[2];
        if (r < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}
