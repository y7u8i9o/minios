#include <wchar.h>

/* Widths from the tables of tools/genunicode.py: 0 for marks, format
 * controls and the Hangul medial and final jamo, 2 for East Asian Width W
 * and F, -1 for controls, surrogates and unassigned code points, and 1 for
 * every other character. */
#include "unidata.h"

static int search(const struct uni_range *t, size_t n, unsigned c)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c < t[mid].first)
            hi = mid;
        else if (c > t[mid].last)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

#define IN_TABLE(t, c) search(t, sizeof t / sizeof t[0], c)

int wcwidth(wchar_t wc)
{
    unsigned c = (unsigned)wc;
    if (!c)
        return 0;
    if (c < 32 || (c >= 0x7f && c < 0xa0) || c > 0x10ffff)
        return -1;
    if (IN_TABLE(uni_zero, c))
        return 0;
    if (!IN_TABLE(uni_print, c))
        return -1;
    return IN_TABLE(uni_wide, c) ? 2 : 1;
}

int wcswidth(const wchar_t *s, size_t n)
{
    int total = 0;
    while (n-- && *s) {
        int w = wcwidth(*s++);
        if (w < 0 || total > 2147483647 - w)
            return -1;
        total += w;
    }
    return total;
}
