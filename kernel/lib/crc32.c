#include <lib/crc32.h>

static uint32_t table[256];
static bool table_ready;

static void make_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    table_ready = true;
}

uint32_t crc32(uint32_t crc, const void *data, size_t n)
{
    if (!table_ready)
        make_table();
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
