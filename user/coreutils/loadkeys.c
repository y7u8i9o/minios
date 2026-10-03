/* loadkeys: set the layout of the console keyboard from a keymap file of
 * /usr/share/keymaps (loadkeys(1), docs/design/keymaps.md).  With -c the
 * layout is the keymap setting of the desktop configuration. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <minios/conf.h>
#include <minios/kbdmap.h>

static unsigned u16(const unsigned char *p)
{
    return (unsigned)(p[0] | p[1] << 8);
}

/* convert fills map from an MKM2 file, or from an MKM1 file as one group
 * of the levels plain and Shift.  Returns 0 or -1. */
static int convert(const unsigned char *p, size_t size, struct kbd_keymap *map)
{
    memset(map, 0, sizeof *map);
    int v2 = size >= 16 && memcmp(p, "MKM2", 4) == 0;
    if (!v2 && (size < 8 || memcmp(p, "MKM1", 4) != 0))
        return -1;
    unsigned entries = u16(p + 4), levels = u16(p + 6);
    unsigned groups = v2 ? u16(p + 8) : 1, ncompose = v2 ? u16(p + 12) : 0;
    size_t header = v2 ? 16 : 8, table = (size_t)groups * entries * levels * 2;
    if (groups < 1 || groups > 2 || levels < 2 || size < header + table + (size_t)ncompose * 6)
        return -1;
    map->groups = (uint16_t)groups;
    map->switch_mode = v2 ? (uint16_t)u16(p + 10) : 0;
    map->uses_altgr = v2 ? (uint16_t)(u16(p + 14) & 1) : 0;
    for (unsigned g = 0; g < groups; g++)
        for (unsigned k = 0; k < KBD_KEYS && k < entries; k++)
            for (unsigned l = 0; l < 4 && l < levels; l++)
                if (v2 || l < 2)
                    map->table[g][k][l] = (uint16_t)u16(p + header + ((g * entries + k) * levels + l) * 2);
    if (ncompose > KBD_COMPOSE_MAX)
        ncompose = KBD_COMPOSE_MAX;
    for (unsigned i = 0; i < ncompose; i++)
        for (unsigned j = 0; j < 3; j++)
            map->compose[i][j] = (uint16_t)u16(p + header + table + (i * 3 + j) * 2);
    map->ncompose = (uint16_t)ncompose;
    return 0;
}

/* configured copies the keymap setting of the configuration into name. */
static int configured(char *name, size_t size)
{
    char path[256];
    FILE *f = fopen(conf_read_path(path, sizeof path), "r");
    if (!f)
        return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "keymap=", 7) == 0 && line[7]) {
            strlcpy(name, line + 7, size);
            found = 0;
        }
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv)
{
    char name[64];
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        if (configured(name, sizeof name) < 0)
            return 0;               /* Without a setting the console retains its layout. */
    } else if (argc == 2 && argv[1][0] != '-' && !strchr(argv[1], '/')) {
        strlcpy(name, argv[1], sizeof name);
    } else {
        fprintf(stderr, "usage: loadkeys NAME | loadkeys -c\n");
        return 2;
    }
    char path[128];
    snprintf(path, sizeof path, "/usr/share/keymaps/%s.mkm", name);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "loadkeys: no keymap %s\n", name);
        return 1;
    }
    static unsigned char buf[65536];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    static struct kbd_keymap map;
    if (convert(buf, n, &map) < 0) {
        fprintf(stderr, "loadkeys: %s is not a keymap file\n", path);
        return 1;
    }
    int fd = open("/dev/console", O_RDWR | O_CLOEXEC);
    if (fd < 0 || ioctl(fd, KBD_SET_KEYMAP, &map) < 0) {
        perror("loadkeys: /dev/console");
        return 1;
    }
    close(fd);
    printf("loadkeys: %s\n", name);
    return 0;
}
