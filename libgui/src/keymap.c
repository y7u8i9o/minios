#include <gui/keymap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

static struct keymap *parse(void *data, size_t size, void *map)
{
    const uint8_t *p = data;
    if (size < 8 || memcmp(p, "MKM1", 4) != 0)
        return NULL;
    int entries = p[4] | p[5] << 8, levels = p[6] | p[7] << 8;
    if (entries <= 0 || levels <= 0 || size < 8 + (size_t)entries * levels * 2)
        return NULL;
    struct keymap *k = calloc(1, sizeof *k);
    if (!k)
        return NULL;
    k->entries = entries;
    k->levels = levels;
    k->table = malloc((size_t)entries * levels * 2);
    if (!k->table) {
        free(k);
        return NULL;
    }
    for (int i = 0; i < entries * levels; i++)
        k->table[i] = (uint16_t)(p[8 + i * 2] | p[9 + i * 2] << 8);
    k->map = map;
    k->size = size;
    return k;
}

struct keymap *keymap_from_fd(int fd, size_t size)
{
    size_t len = (size + 4095) & ~(size_t)4095;
    void *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        return NULL;
    struct keymap *k = parse(map, size, NULL);
    munmap(map, len);
    return k;
}

struct keymap *keymap_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    uint8_t buf[8 + 256 * 4 * 2 + 16];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return parse(buf, n, NULL);
}

void keymap_free(struct keymap *k)
{
    if (k) {
        free(k->table);
        free(k);
    }
}

int keymap_translate(const struct keymap *k, uint32_t key, int modifiers)
{
    if (!k || key >= (uint32_t)k->entries)
        return 0;
    int level = (modifiers & KEYMAP_MOD_ALT) ? 3 : (modifiers & KEYMAP_MOD_CTRL) ? 2 : (modifiers & KEYMAP_MOD_SHIFT) ? 1 : 0;
    if (level >= k->levels)
        level = 0;
    return k->table[key * k->levels + level];
}
