/* Base64 decoding (minios/base64.h). */
#include <minios/base64.h>

static int value_of(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

long base64_decode(const char *text, size_t len, uint8_t *out)
{
    uint32_t group = 0;
    int count = 0, pad = 0;
    long n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == '=') {
            pad++;
            group <<= 6;
            count++;
        } else {
            int v = value_of(c);
            if (v < 0 || pad)
                return -1;
            group = group << 6 | (uint32_t)v;
            count++;
        }
        if (count == 4) {
            if (pad > 2)
                return -1;
            out[n++] = (uint8_t)(group >> 16);
            if (pad < 2)
                out[n++] = (uint8_t)(group >> 8);
            if (pad < 1)
                out[n++] = (uint8_t)group;
            group = 0;
            count = 0;
            if (pad)
                pad = 3;        /* no group may follow a padded group */
        }
    }
    return count ? -1 : n;
}
