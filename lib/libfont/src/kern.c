/* Kerning from the kern table (format 0) and GPOS pair adjustment
 * (lookup type 2, formats 1 and 2, also through extension lookups) of
 * the kern feature, and simple shaping of ISO 8859-1 strings. */
#include "internal.h"
#include <stdlib.h>

static const uint8_t *at(const struct ofont *f, uint32_t off, uint32_t need)
{
    return off <= f->size && need <= f->size - off ? f->data + off : NULL;
}

/* ---- kern table ---- */

int kern_table(const struct ofont *f, int left, int right, int *found)
{
    *found = 0;
    const uint8_t *t = at(f, f->kern, 4);
    if (!t || rd16(t) != 0)
        return 0;
    int ntables = rd16(t + 2);
    uint32_t off = f->kern + 4;
    for (int i = 0; i < ntables; i++) {
        const uint8_t *st = at(f, off, 6);
        if (!st)
            return 0;
        uint32_t len = rd16(st + 2);
        int coverage = rd16(st + 4);
        if ((coverage >> 8) == 0 && (coverage & 1)) {
            const uint8_t *p = at(f, off + 6, 8);
            if (!p)
                return 0;
            int npairs = rd16(p);
            const uint8_t *pairs = at(f, off + 14, (uint32_t)npairs * 6);
            if (!pairs)
                return 0;
            uint32_t key = (uint32_t)left << 16 | (uint32_t)right;
            int lo = 0, hi = npairs - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                uint32_t k = rd32(pairs + mid * 6);
                if (k == key) {
                    *found = 1;
                    return rds16(pairs + mid * 6 + 4);
                }
                if (k < key)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
        }
        if (len == 0)
            break;
        off += len;
    }
    return 0;
}

/* ---- GPOS ---- */

/* Index of glyph in a coverage table, -1 when absent. */
static int coverage_index(const struct ofont *f, uint32_t off, int glyph)
{
    const uint8_t *c = at(f, off, 4);
    if (!c)
        return -1;
    int format = rd16(c), n = rd16(c + 2);
    if (format == 1) {
        const uint8_t *g = at(f, off + 4, (uint32_t)n * 2);
        if (!g)
            return -1;
        int lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2, v = rd16(g + mid * 2);
            if (v == glyph)
                return mid;
            if (v < glyph)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        return -1;
    }
    if (format == 2) {
        const uint8_t *r = at(f, off + 4, (uint32_t)n * 6);
        if (!r)
            return -1;
        for (int i = 0; i < n; i++) {
            int s = rd16(r + i * 6), e = rd16(r + i * 6 + 2);
            if (glyph >= s && glyph <= e)
                return rd16(r + i * 6 + 4) + glyph - s;
        }
    }
    return -1;
}

static int class_of(const struct ofont *f, uint32_t off, int glyph)
{
    const uint8_t *c = at(f, off, 4);
    if (!c)
        return 0;
    int format = rd16(c);
    if (format == 1) {
        int start = rd16(c + 2);
        const uint8_t *p = at(f, off + 4, 2);
        if (!p)
            return 0;
        int n = rd16(p);
        if (glyph < start || glyph >= start + n)
            return 0;
        const uint8_t *v = at(f, off + 6 + (uint32_t)(glyph - start) * 2, 2);
        return v ? rd16(v) : 0;
    }
    if (format == 2) {
        int n = rd16(c + 2);
        const uint8_t *r = at(f, off + 4, (uint32_t)n * 6);
        if (!r)
            return 0;
        for (int i = 0; i < n; i++) {
            int s = rd16(r + i * 6), e = rd16(r + i * 6 + 2);
            if (glyph >= s && glyph <= e)
                return rd16(r + i * 6 + 4);
        }
    }
    return 0;
}

static int value_size(int format)
{
    int n = 0;
    for (int b = format; b; b >>= 1)
        n += b & 1;
    return n * 2;
}

/* XAdvance out of a value record, 0 when the format lacks it. */
static int value_xadvance(const uint8_t *rec, int format)
{
    if (!(format & 4))
        return 0;
    int idx = (format & 1) + ((format >> 1) & 1);
    return rds16(rec + idx * 2);
}

static int pairpos_kern(const struct ofont *f, uint32_t off, int left, int right, int *found)
{
    const uint8_t *st = at(f, off, 10);
    if (!st)
        return 0;
    int format = rd16(st);
    int ci = coverage_index(f, off + rd16(st + 2), left);
    if (ci < 0)
        return 0;
    int vf1 = rd16(st + 4), vf2 = rd16(st + 6);
    int recsize = value_size(vf1) + value_size(vf2);
    if (format == 1) {
        int nsets = rd16(st + 8);
        if (ci >= nsets)
            return 0;
        const uint8_t *so = at(f, off + 10 + (uint32_t)ci * 2, 2);
        if (!so)
            return 0;
        uint32_t set = off + rd16(so);
        const uint8_t *s = at(f, set, 2);
        if (!s)
            return 0;
        int n = rd16(s);
        for (int i = 0; i < n; i++) {
            const uint8_t *r = at(f, set + 2 + (uint32_t)i * (2 + recsize), (uint32_t)(2 + recsize));
            if (!r)
                return 0;
            if (rd16(r) == right) {
                *found = 1;
                return value_xadvance(r + 2, vf1);
            }
        }
        return 0;
    }
    if (format == 2) {
        const uint8_t *h = at(f, off, 16);
        if (!h)
            return 0;
        int c1 = class_of(f, off + rd16(h + 8), left), c2 = class_of(f, off + rd16(h + 10), right);
        int n1 = rd16(h + 12), n2 = rd16(h + 14);
        if (c1 >= n1 || c2 >= n2)
            return 0;
        const uint8_t *r = at(f, off + 16 + ((uint32_t)c1 * n2 + c2) * recsize, (uint32_t)recsize);
        if (!r)
            return 0;
        *found = 1;
        return value_xadvance(r, vf1);
    }
    return 0;
}

static int lookup_kern(const struct ofont *f, uint32_t lookup_list, int index, int left, int right, int *found)
{
    const uint8_t *ll = at(f, lookup_list, 2);
    if (!ll || index >= rd16(ll))
        return 0;
    const uint8_t *lo = at(f, lookup_list + 2 + (uint32_t)index * 2, 2);
    if (!lo)
        return 0;
    uint32_t lookup = lookup_list + rd16(lo);
    const uint8_t *l = at(f, lookup, 6);
    if (!l)
        return 0;
    int type = rd16(l), nsub = rd16(l + 4);
    for (int i = 0; i < nsub; i++) {
        const uint8_t *so = at(f, lookup + 6 + (uint32_t)i * 2, 2);
        if (!so)
            return 0;
        uint32_t sub = lookup + rd16(so);
        int t = type;
        if (t == 9) {
            const uint8_t *e = at(f, sub, 8);
            if (!e)
                return 0;
            t = rd16(e + 2);
            sub += rd32(e + 4);
        }
        if (t != 2)
            continue;
        int v = pairpos_kern(f, sub, left, right, found);
        if (*found)
            return v;
    }
    return 0;
}

int gpos_kern(const struct ofont *f, int left, int right, int *found)
{
    *found = 0;
    const uint8_t *g = at(f, f->gpos, 10);
    if (!g)
        return 0;
    uint32_t features = f->gpos + rd16(g + 6), lookups = f->gpos + rd16(g + 8);
    const uint8_t *fl = at(f, features, 2);
    if (!fl)
        return 0;
    int nf = rd16(fl);
    for (int i = 0; i < nf; i++) {
        const uint8_t *fr = at(f, features + 2 + (uint32_t)i * 6, 6);
        if (!fr)
            return 0;
        if (memcmp(fr, "kern", 4) != 0)
            continue;
        uint32_t feat = features + rd16(fr + 4);
        const uint8_t *ft = at(f, feat, 4);
        if (!ft)
            return 0;
        int nl = rd16(ft + 2);
        for (int j = 0; j < nl; j++) {
            const uint8_t *li = at(f, feat + 4 + (uint32_t)j * 2, 2);
            if (!li)
                return 0;
            int v = lookup_kern(f, lookups, rd16(li), left, right, found);
            if (*found)
                return v;
        }
    }
    return 0;
}

int font_kern(const struct ofont *f, int left, int right)
{
    int found = 0, v = 0;
    if (f->gpos) {
        v = gpos_kern(f, left, right, &found);
        if (found)
            return v;
    }
    if (f->kern)
        v = kern_table(f, left, right, &found);
    return found ? v : 0;
}

/* ---- shaping ---- */

static uint32_t utf8_next(const char *s, int n, int *at)
{
    int i = *at;
    unsigned c = (unsigned char)s[i++];
    if (c < 0x80) {
        *at = i;
        return c;
    }
    int need = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
    uint32_t cp = need == 1 ? c & 0x1f : need == 2 ? c & 0x0f : need == 3 ? c & 7 : 0xfffd;
    if (need < 0) {
        *at = i;
        return 0xfffd;
    }
    for (int k = 0; k < need; k++) {
        if (i >= n || ((unsigned char)s[i] & 0xc0) != 0x80) {
            *at = i;
            return 0xfffd;
        }
        cp = cp << 6 | ((unsigned char)s[i++] & 0x3f);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        *at = i;
        return 0xfffd;
    }
    *at = i;
    return cp;
}

int font_shape(const struct ofont *f, const char *text, int n, int px, struct font_shaped *out, int max, int32_t *width)
{
    int32_t pen = 0;
    int prev = 0, count = 0;
    int len = n < 0 ? (int)strlen(text) : n;
    for (int i = 0; i < len;) {
        int byte = i;
        uint32_t cp = utf8_next(text, len, &i);
        int g = font_glyph_index(f, cp);
        if (prev && g)
            pen += font_scale(f, font_kern(f, prev, g), px);
        if (count < max) {
            out[count].glyph = g;
            out[count].x = pen;
            out[count].byte = byte;
            out[count].codepoint = cp;
        }
        count++;
        pen += font_scale(f, font_advance(f, g), px);
        prev = g;
    }
    if (width)
        *width = pen;
    return count < max ? count : max;
}
