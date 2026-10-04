/* GUIDs in their stored byte order (lib/guid.h): the partition GUIDs of
 * GPT, the boot disk GUIDs of Limine and the system UUID of SMBIOS. */
#include <lib/guid.h>
#include <lib/string.h>
#include <errno.h>

/* The stored byte of each position of the string form. */
static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };

void guid_format(const uint8_t g[16], char out[GUID_STR])
{
    static const char hexdigits[] = "0123456789abcdef";
    char *p = out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            *p++ = '-';
        *p++ = hexdigits[g[order[i]] >> 4];
        *p++ = hexdigits[g[order[i]] & 15];
    }
    *p = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int guid_parse(const char *s, uint8_t out[16])
{
    if (strlen(s) != 36)
        return -EINVAL;
    for (int i = 0, at = 0; i < 16; i++) {
        if (at == 8 || at == 13 || at == 18 || at == 23) {
            if (s[at] != '-')
                return -EINVAL;
            at++;
        }
        int hi = hexval(s[at]), lo = hexval(s[at + 1]);
        if (hi < 0 || lo < 0)
            return -EINVAL;
        out[order[i]] = (uint8_t)(hi << 4 | lo);
        at += 2;
    }
    return 0;
}

bool guid_is_zero(const uint8_t g[16])
{
    for (int i = 0; i < 16; i++)
        if (g[i])
            return false;
    return true;
}
