/* CRC-32 with a table of 256 entries. The table is computed at the first
 * call. The last entry is stored last, with release order, and marks the
 * table as complete. Two threads that compute the table at the same time
 * write the same values, so the computation needs no lock. Host tools
 * compile this file with the host libc. */
#include <minios/crc32.h>

static uint32_t table[256];

static void make_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        if (i < 255)
            table[i] = c;
        else
            __atomic_store_n(&table[i], c, __ATOMIC_RELEASE);
    }
}

uint32_t crc32(uint32_t crc, const void *data, size_t n)
{
    if (!__atomic_load_n(&table[255], __ATOMIC_ACQUIRE))
        make_table();
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
