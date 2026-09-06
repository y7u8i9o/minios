#include <wctype.h>
#include <string.h>

static int in(wint_t c, wint_t lo, wint_t hi)
{
    return c >= lo && c <= hi;
}

/* Letters outside ASCII: the Latin-1 letters, Latin Extended-A and B,
 * Greek and Cyrillic. The two Latin-1 signs 0xd7 and 0xf7 are excluded. */
static int letter(wint_t c)
{
    if (c < 0x80)
        return in(c, 'a', 'z') || in(c, 'A', 'Z');
    if (in(c, 0xc0, 0x24f))
        return c != 0xd7 && c != 0xf7;
    if (c == 0xaa || c == 0xb5 || c == 0xba)
        return 1;
    return in(c, 0x370, 0x373) || in(c, 0x376, 0x377) || in(c, 0x37b, 0x37d)
        || in(c, 0x386, 0x3ff) || in(c, 0x400, 0x481) || in(c, 0x48a, 0x52f);
}

int iswcntrl(wint_t c)
{
    return c < 0x20 || c == 0x7f || in(c, 0x80, 0x9f);
}

int iswspace(wint_t c)
{
    return c == ' ' || in(c, '\t', '\r') || c == 0x85 || c == 0xa0 || c == 0x1680
        || in(c, 0x2000, 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f
        || c == 0x205f || c == 0x3000;
}

int iswblank(wint_t c)
{
    return c == ' ' || c == '\t' || c == 0xa0 || c == 0x1680 || in(c, 0x2000, 0x200a)
        || c == 0x202f || c == 0x205f || c == 0x3000;
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
    return letter(c);
}

int iswalnum(wint_t c)
{
    return letter(c) || iswdigit(c);
}

int iswupper(wint_t c)
{
    if (c < 0x80)
        return in(c, 'A', 'Z');
    if (in(c, 0xc0, 0xde))
        return c != 0xd7;
    if (in(c, 0x100, 0x17f) || in(c, 0x460, 0x481) || in(c, 0x48a, 0x4bf) || in(c, 0x4d0, 0x52f))
        return (c & 1) == 0 && letter(c);
    if (in(c, 0x391, 0x3ab))
        return c != 0x3a2;
    return in(c, 0x400, 0x42f);
}

int iswlower(wint_t c)
{
    if (c < 0x80)
        return in(c, 'a', 'z');
    if (in(c, 0xdf, 0xff))
        return c != 0xf7;
    if (c == 0xaa || c == 0xb5 || c == 0xba)
        return 1;
    if (in(c, 0x100, 0x17f) || in(c, 0x460, 0x481) || in(c, 0x48a, 0x4bf) || in(c, 0x4d0, 0x52f))
        return (c & 1) == 1 && letter(c);
    if (in(c, 0x3b1, 0x3ce))
        return 1;
    return in(c, 0x430, 0x45f);
}

int iswprint(wint_t c)
{
    return !iswcntrl(c) && c <= 0x10ffff && !in(c, 0xd800, 0xdfff);
}

int iswgraph(wint_t c)
{
    return iswprint(c) && !iswspace(c);
}

int iswpunct(wint_t c)
{
    return iswgraph(c) && !iswalnum(c);
}

wint_t towupper(wint_t c)
{
    if (in(c, 'a', 'z'))
        return c - 0x20;
    if (in(c, 0xe0, 0xfe) && c != 0xf7)
        return c - 0x20;
    if (c == 0xff)
        return 0x178;
    if (in(c, 0x100, 0x17f) && (c & 1) == 1 && letter(c) && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17f)
        return c - 1;
    if (in(c, 0x3b1, 0x3c1) || in(c, 0x3c3, 0x3cb))
        return c - 0x20;
    if (c == 0x3c2)
        return 0x3a3;
    if (in(c, 0x430, 0x44f))
        return c - 0x20;
    if (in(c, 0x450, 0x45f))
        return c - 0x50;
    if ((in(c, 0x460, 0x481) || in(c, 0x48a, 0x4bf) || in(c, 0x4d0, 0x52f)) && (c & 1) == 1)
        return c - 1;
    return c;
}

wint_t towlower(wint_t c)
{
    if (in(c, 'A', 'Z'))
        return c + 0x20;
    if (in(c, 0xc0, 0xde) && c != 0xd7)
        return c + 0x20;
    if (c == 0x178)
        return 0xff;
    if (in(c, 0x100, 0x17f) && (c & 1) == 0 && letter(c) && c != 0x130 && c != 0x138)
        return c + 1;
    if (in(c, 0x391, 0x3a1) || in(c, 0x3a3, 0x3ab))
        return c + 0x20;
    if (in(c, 0x410, 0x42f))
        return c + 0x20;
    if (in(c, 0x400, 0x40f))
        return c + 0x50;
    if ((in(c, 0x460, 0x481) || in(c, 0x48a, 0x4bf) || in(c, 0x4d0, 0x52f)) && (c & 1) == 0)
        return c + 1;
    return c;
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
