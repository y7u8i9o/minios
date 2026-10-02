/* Collation (L1, docs/design/locale.md).
 *
 * A locale with collate "latin" compares text in four levels: the base
 * letters, then the accents, then the case, then the code points.  The
 * base letter and the accent of a precomposed letter come from its
 * canonical decomposition (uni_bases in unidata.h).  Spaces, punctuation
 * and symbols sort before digits, digits before letters, and Latin before
 * Greek, Cyrillic and the other scripts, which compare by code point.
 * collate_after lists letters that sort after another letter as letters
 * of their own, such as "n ñ" in Spanish.  The C locale and the locales
 * with collate "codepoint" compare bytes, as strcmp does.
 *
 * strcoll builds the sort keys of both strings, as strxfrm returns them,
 * and compares the keys. */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "locale_impl.h"
#include "../wchar/unidata.h"

/* A sort key is a byte string without zero bytes.  Each weight takes four
 * bytes from 2 to 255, and the byte 1 separates the levels. */
struct key {
    unsigned char *buf;
    size_t len, cap;
};

static int key_put(struct key *k, unsigned char b)
{
    if (k->len + 1 >= k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 64;
        unsigned char *n = realloc(k->buf, cap);
        if (!n)
            return -1;
        k->buf = n;
        k->cap = cap;
    }
    k->buf[k->len++] = b;
    return 0;
}

static int key_weight(struct key *k, uint32_t w)
{
    unsigned char d[4];
    for (int i = 3; i >= 0; i--) {
        d[i] = (unsigned char)(2 + w % 254);
        w /= 254;
    }
    for (int i = 0; i < 4; i++)
        if (key_put(k, d[i]) < 0)
            return -1;
    return 0;
}

static const struct uni_base *base_of(uint32_t cp)
{
    size_t lo = 0, hi = sizeof uni_bases / sizeof uni_bases[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < uni_bases[mid].cp)
            hi = mid;
        else if (cp > uni_bases[mid].cp)
            lo = mid + 1;
        else
            return &uni_bases[mid];
    }
    return NULL;
}

static uint32_t decode(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (extra)
        c &= 0x3f >> extra;
    while (extra-- > 0 && (*p & 0xc0) == 0x80)
        c = c << 6 | (*p++ & 0x3f);
    *s = (const char *)p;
    return c;
}

/* The tailoring of collate_after: up to 8 letters that sort right after
 * another letter. */
struct tailoring {
    uint32_t base[8], letter[8];
    int count;
};

static void read_tailoring(const char *list, struct tailoring *t)
{
    t->count = 0;
    while (*list && t->count < 8) {
        while (*list == ' ' || *list == ';')
            list++;
        if (!*list)
            break;
        uint32_t base = decode(&list);
        while (*list == ' ')
            list++;
        if (!*list || *list == ';')
            break;
        t->base[t->count] = base;
        t->letter[t->count] = decode(&list);
        t->count++;
    }
}

/* primary returns the primary weight of a folded letter or character: 0
 * for marks and other characters of width 0, which do not take part in
 * the first level. */
static uint32_t primary(uint32_t c)
{
    if (c && wcwidth((wchar_t)c) == 0)
        return 0;
    if (c >= '0' && c <= '9')
        return 0x1000000 + (c - '0');
    if (!iswalpha(c))
        return 1 + c;
    if (c < 0x250 || (c >= 0x1e00 && c <= 0x1eff))
        return 0x2000000 + c * 2;
    if (c >= 0x370 && c <= 0x3ff)
        return 0x3000000 + c * 2;
    if (c >= 0x400 && c <= 0x52f)
        return 0x4000000 + c * 2;
    return 0x5000000 + c * 2;
}

/* build_key writes the sort key of n code points. */
static int build_key(const uint32_t *cp, size_t n, const struct tailoring *t, struct key *k)
{
    uint32_t *sec = malloc((n + 1) * sizeof *sec);
    unsigned char *ter = malloc(n + 1);
    if (!sec || !ter) {
        free(sec);
        free(ter);
        return -1;
    }
    int r = 0;
    for (size_t i = 0; i < n && r == 0; i++) {
        uint32_t c = cp[i], mark = 0;
        const struct uni_base *b = base_of(c);
        if (b) {
            c = b->base;
            mark = b->mark;
        }
        ter[i] = iswupper(c) ? 1 : 0;
        c = towlower(c);
        uint32_t folded = towlower(cp[i]), p = primary(c);
        for (int j = 0; j < t->count; j++)
            if (folded == t->letter[j]) {
                p = primary(t->base[j]) + 1;
                mark = 0;
            }
        if (p == 0) {
            sec[i] = c;
            continue;
        }
        sec[i] = mark;
        r = key_weight(k, p);
    }
    if (r == 0)
        r = key_put(k, 1);
    for (size_t i = 0; i < n && r == 0; i++)
        r = key_weight(k, sec[i]);
    if (r == 0)
        r = key_put(k, 1);
    for (size_t i = 0; i < n && r == 0; i++)
        r = key_put(k, (unsigned char)(2 + ter[i]));
    if (r == 0)
        r = key_put(k, 1);
    for (size_t i = 0; i < n && r == 0; i++)
        r = key_weight(k, cp[i]);
    if (r == 0)
        k->buf[k->len] = '\0';
    free(sec);
    free(ter);
    return r;
}

static int utf8_key(const char *s, const struct locale_data *d, struct key *k)
{
    size_t n = 0, cap = strlen(s) + 1;
    uint32_t *cp = malloc(cap * sizeof *cp);
    if (!cp)
        return -1;
    while (*s)
        cp[n++] = decode(&s);
    struct tailoring t;
    read_tailoring(d->str[LI_COLLATE_AFTER], &t);
    int r = build_key(cp, n, &t, k);
    free(cp);
    return r;
}

static int wide_key(const wchar_t *s, const struct locale_data *d, struct key *k)
{
    size_t n = wcslen(s);
    uint32_t *cp = malloc((n + 1) * sizeof *cp);
    if (!cp)
        return -1;
    for (size_t i = 0; i < n; i++)
        cp[i] = (uint32_t)s[i];
    struct tailoring t;
    read_tailoring(d->str[LI_COLLATE_AFTER], &t);
    int r = build_key(cp, n, &t, k);
    free(cp);
    return r;
}

static const struct locale_data *collation(locale_t loc)
{
    if (loc == LC_GLOBAL_LOCALE)
        loc = &__global_locale;
    return loc->cat[LC_COLLATE];
}

int strcoll_l(const char *a, const char *b, locale_t loc)
{
    const struct locale_data *d = collation(loc);
    if (!d->collate_latin)
        return strcmp(a, b);
    struct key ka = { 0 }, kb = { 0 };
    int r;
    if (utf8_key(a, d, &ka) == 0 && utf8_key(b, d, &kb) == 0)
        r = strcmp((char *)ka.buf, (char *)kb.buf);
    else
        r = strcmp(a, b);
    free(ka.buf);
    free(kb.buf);
    return r;
}

int strcoll(const char *a, const char *b)
{
    return strcoll_l(a, b, __locale_current());
}

size_t strxfrm_l(char *dst, const char *src, size_t n, locale_t loc)
{
    const struct locale_data *d = collation(loc);
    size_t len;
    if (!d->collate_latin) {
        len = strlen(src);
        if (len < n)
            memcpy(dst, src, len + 1);
        return len;
    }
    struct key k = { 0 };
    if (utf8_key(src, d, &k) < 0) {
        free(k.buf);
        return (size_t)-1;
    }
    len = k.len;
    if (len < n)
        memcpy(dst, k.buf, len + 1);
    free(k.buf);
    return len;
}

size_t strxfrm(char *dst, const char *src, size_t n)
{
    return strxfrm_l(dst, src, n, __locale_current());
}

int wcscoll(const wchar_t *a, const wchar_t *b)
{
    const struct locale_data *d = collation(__locale_current());
    if (!d->collate_latin)
        return wcscmp(a, b);
    struct key ka = { 0 }, kb = { 0 };
    int r;
    if (wide_key(a, d, &ka) == 0 && wide_key(b, d, &kb) == 0)
        r = strcmp((char *)ka.buf, (char *)kb.buf);
    else
        r = wcscmp(a, b);
    free(ka.buf);
    free(kb.buf);
    return r;
}

size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n)
{
    const struct locale_data *d = collation(__locale_current());
    size_t len;
    if (!d->collate_latin) {
        len = wcslen(src);
        if (len < n)
            wmemcpy(dst, src, len + 1);
        return len;
    }
    struct key k = { 0 };
    if (wide_key(src, d, &k) < 0) {
        free(k.buf);
        return (size_t)-1;
    }
    len = k.len;
    if (len < n)
        for (size_t i = 0; i <= len; i++)
            dst[i] = k.buf[i];
    free(k.buf);
    return len;
}
