/* Character classes and case mappings from the tables that
 * tools/genunicode.py generates from the Unicode Character Database. The
 * ASCII classes follow POSIX: digits are 0 to 9 only. The no-break spaces
 * U+00A0, U+2007 and U+202F are printable and neither space nor graph. */
#include <wctype.h>
#include <string.h>
#include "unidata.h"

static int in(wint_t c, wint_t lo, wint_t hi)
{
    return c >= lo && c <= hi;
}

/* uni_in searches a sorted table of ranges. */
static int uni_in(const struct uni_range *t, size_t n, wint_t c)
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

#define IN_TABLE(t, c) uni_in(t, sizeof t / sizeof t[0], c)

/* uni_map applies the case run that contains c, if any. */
static wint_t uni_map(const struct uni_case *t, size_t n, wint_t c)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c < t[mid].first)
            hi = mid;
        else if (c > t[mid].last)
            lo = mid + 1;
        else
            return (c - t[mid].first) % t[mid].stride == 0 ? (wint_t)((int32_t)c + t[mid].delta) : c;
    }
    return c;
}

int iswcntrl(wint_t c)
{
    return c < 0x20 || c == 0x7f || in(c, 0x80, 0x9f) || c == 0x2028 || c == 0x2029;
}

int iswspace(wint_t c)
{
    return c == ' ' || in(c, '\t', '\r') || c == 0x85 || c == 0x1680 || in(c, 0x2000, 0x2006)
        || in(c, 0x2008, 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x205f || c == 0x3000;
}

int iswblank(wint_t c)
{
    return c == ' ' || c == '\t' || c == 0x1680 || in(c, 0x2000, 0x2006) || in(c, 0x2008, 0x200a)
        || c == 0x205f || c == 0x3000;
}

int iswdigit(wint_t c)
{
    return in(c, '0', '9');
}

int iswxdigit(wint_t c)
{
    return in(c, '0', '9') || in(c, 'a', 'f') || in(c, 'A', 'F');
}

int iswalpha(wint_t c)
{
    return IN_TABLE(uni_alpha, c);
}

int iswalnum(wint_t c)
{
    return iswalpha(c) || iswdigit(c);
}

int iswupper(wint_t c)
{
    return IN_TABLE(uni_upper, c);
}

int iswlower(wint_t c)
{
    return IN_TABLE(uni_lower, c);
}

int iswprint(wint_t c)
{
    return !iswcntrl(c) && IN_TABLE(uni_print, c);
}

int iswgraph(wint_t c)
{
    return iswprint(c) && !iswspace(c) && c != 0xa0 && c != 0x2007 && c != 0x202f;
}

int iswpunct(wint_t c)
{
    return IN_TABLE(uni_punct, c);
}

wint_t towupper(wint_t c)
{
    return uni_map(uni_toupper, sizeof uni_toupper / sizeof uni_toupper[0], c);
}

wint_t towlower(wint_t c)
{
    return uni_map(uni_tolower, sizeof uni_tolower / sizeof uni_tolower[0], c);
}

static const struct {
    const char *name;
    int (*test)(wint_t);
} classes[] = {
    { "alnum", iswalnum }, { "alpha", iswalpha }, { "blank", iswblank },
    { "cntrl", iswcntrl }, { "digit", iswdigit }, { "graph", iswgraph },
    { "lower", iswlower }, { "print", iswprint }, { "punct", iswpunct },
    { "space", iswspace }, { "upper", iswupper }, { "xdigit", iswxdigit },
};

/* A class handle is its index plus one; 0 means an unknown name. */
wctype_t wctype(const char *name)
{
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if (strcmp(classes[i].name, name) == 0)
            return i + 1;
    return 0;
}

int iswctype(wint_t c, wctype_t type)
{
    if (type == 0 || type > sizeof classes / sizeof classes[0])
        return 0;
    return classes[type - 1].test(c);
}
