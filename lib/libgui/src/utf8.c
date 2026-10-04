#include <gui/utf8.h>

uint32_t gui_utf8_decode(const char *s, int len, int *at)
{
    int i = *at;
    if (!s || i < 0 || i >= len)
        return 0;
    unsigned c = (unsigned char)s[i++];
    if (c < 0x80) {
        *at = i;
        return c;
    }
    int need = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 :
               c >= 0xf0 && c <= 0xf4 ? 3 : -1;
    uint32_t cp = need == 1 ? c & 0x1f : need == 2 ? c & 0x0f : need == 3 ? c & 7 : 0xfffd;
    if (need < 0) {
        *at = i;
        return 0xfffd;
    }
    for (int k = 0; k < need; k++) {
        if (i >= len || ((unsigned char)s[i] & 0xc0) != 0x80) {
            *at = i;
            return 0xfffd;
        }
        cp = cp << 6 | ((unsigned char)s[i++] & 0x3f);
    }
    if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
        (need == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff))
        cp = 0xfffd;
    *at = i;
    return cp;
}

int gui_utf8_encode(uint32_t cp, char out[4])
{
    if (cp <= 0x7f) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7ff) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp >= 0xd800 && cp <= 0xdfff)
        cp = 0xfffd;
    if (cp <= 0xffff) {
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    if (cp > 0x10ffff)
        cp = 0xfffd;
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

int gui_utf8_next_boundary(const char *s, int len, int at)
{
    if (at < 0) at = 0;
    if (at >= len) return len;
    gui_utf8_decode(s, len, &at);
    return at;
}

int gui_utf8_prev_boundary(const char *s, int at)
{
    if (at <= 0)
        return 0;
    at--;
    while (at > 0 && ((unsigned char)s[at] & 0xc0) == 0x80)
        at--;
    return at;
}

