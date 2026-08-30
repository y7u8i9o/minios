/* CFF: DICT and INDEX structures, and a Type 2 charstring interpreter
 * producing outlines with cubic control points. Hints are skipped. */
#include "internal.h"
#include <stdlib.h>
#include <errno.h>

struct index {
    uint32_t off;               /* of the INDEX itself */
    int count;
    int off_size;
    uint32_t data;              /* first byte of the object data */
    uint32_t end;               /* first byte after the INDEX */
};

static const uint8_t *at(const struct ofont *f, uint32_t off, uint32_t need)
{
    return off <= f->size && need <= f->size - off ? f->data + off : NULL;
}

static uint32_t rd_off(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v = v << 8 | p[i];
    return v;
}

static int index_read(const struct ofont *f, uint32_t off, struct index *ix)
{
    const uint8_t *p = at(f, off, 2);
    if (!p)
        return -EINVAL;
    ix->off = off;
    ix->count = rd16(p);
    if (ix->count == 0) {
        ix->off_size = 0;
        ix->data = ix->end = off + 2;
        return 0;
    }
    p = at(f, off, 3);
    if (!p)
        return -EINVAL;
    ix->off_size = p[2];
    if (ix->off_size < 1 || ix->off_size > 4)
        return -EINVAL;
    uint32_t offsets = off + 3;
    const uint8_t *last = at(f, offsets + (uint32_t)ix->count * ix->off_size, ix->off_size);
    if (!last)
        return -EINVAL;
    ix->data = offsets + (uint32_t)(ix->count + 1) * ix->off_size - 1;
    ix->end = ix->data + rd_off(last, ix->off_size);
    return ix->end <= f->size ? 0 : -EINVAL;
}

/* Object i of an INDEX: offset and length. */
static int index_get(const struct ofont *f, const struct index *ix, int i, uint32_t *off, uint32_t *len)
{
    if (i < 0 || i >= ix->count)
        return -EINVAL;
    const uint8_t *p = at(f, ix->off + 3 + (uint32_t)i * ix->off_size, (uint32_t)ix->off_size * 2);
    if (!p)
        return -EINVAL;
    uint32_t a = rd_off(p, ix->off_size), b = rd_off(p + ix->off_size, ix->off_size);
    if (b < a || ix->data + b > f->size)
        return -EINVAL;
    *off = ix->data + a;
    *len = b - a;
    return 0;
}

/* DICT parsing: operands are collected until an operator; the caller
 * gets the operator code (two byte operators as 1200 + second byte). */
struct dict_iter {
    const uint8_t *p, *end;
    int32_t operands[48];
    int n;
};

static int dict_next(struct dict_iter *it, int *op)
{
    it->n = 0;
    while (it->p < it->end) {
        int b0 = *it->p++;
        if (b0 <= 21) {
            if (b0 == 12) {
                if (it->p >= it->end)
                    return 0;
                *op = 1200 + *it->p++;
            } else {
                *op = b0;
            }
            return 1;
        }
        int32_t v = 0;
        if (b0 == 28) {
            if (it->p + 2 > it->end) return 0;
            v = rds16(it->p);
            it->p += 2;
        } else if (b0 == 29) {
            if (it->p + 4 > it->end) return 0;
            v = (int32_t)rd32(it->p);
            it->p += 4;
        } else if (b0 == 30) {
            /* Real number: skipped, treated as zero. */
            while (it->p < it->end) {
                int b = *it->p++;
                if ((b & 0x0f) == 0x0f || (b >> 4) == 0x0f)
                    break;
            }
        } else if (b0 >= 32 && b0 <= 246) {
            v = b0 - 139;
        } else if (b0 >= 247 && b0 <= 250) {
            if (it->p >= it->end) return 0;
            v = (b0 - 247) * 256 + *it->p++ + 108;
        } else if (b0 >= 251 && b0 <= 254) {
            if (it->p >= it->end) return 0;
            v = -(b0 - 251) * 256 - *it->p++ - 108;
        } else {
            return 0;
        }
        if (it->n < 48)
            it->operands[it->n++] = v;
    }
    return 0;
}

/* Local subroutines of a Private DICT at (off, len). */
static uint32_t private_subrs(const struct ofont *f, uint32_t off, uint32_t len)
{
    const uint8_t *p = at(f, off, len);
    if (!p)
        return 0;
    struct dict_iter it = { .p = p, .end = p + len };
    int op;
    while (dict_next(&it, &op))
        if (op == 19 && it.n >= 1)
            return off + (uint32_t)it.operands[0];
    return 0;
}

int cff_parse(struct ofont *f, uint32_t off, uint32_t len)
{
    const uint8_t *h = at(f, off, 4);
    if (!h || h[0] != 1)
        return -EINVAL;
    uint32_t pos = off + h[2];
    struct index names, tops, strings, gsubrs;
    if (index_read(f, pos, &names) < 0 || index_read(f, names.end, &tops) < 0 ||
        index_read(f, tops.end, &strings) < 0 || index_read(f, strings.end, &gsubrs) < 0)
        return -EINVAL;
    f->cff.base = off;
    f->cff.gsubrs = gsubrs.count ? gsubrs.off : 0;
    uint32_t toff, tlen;
    if (index_get(f, &tops, 0, &toff, &tlen) < 0)
        return -EINVAL;
    struct dict_iter it = { .p = f->data + toff, .end = f->data + toff + tlen };
    int op;
    uint32_t priv_off = 0, priv_len = 0, charstrings = 0;
    int cstype = 2;
    while (dict_next(&it, &op)) {
        if (op == 17 && it.n >= 1) charstrings = (uint32_t)it.operands[0];
        else if (op == 18 && it.n >= 2) { priv_len = (uint32_t)it.operands[0]; priv_off = (uint32_t)it.operands[1]; }
        else if (op == 1206 && it.n >= 1) cstype = it.operands[0];
        else if (op == 1230) f->cff.is_cid = 1;
        else if (op == 1236 && it.n >= 1) f->cff.fdarray = (uint32_t)it.operands[0];
        else if (op == 1237 && it.n >= 1) f->cff.fdselect = (uint32_t)it.operands[0];
    }
    if (cstype != 2 || !charstrings)
        return -EINVAL;
    struct index cs;
    if (index_read(f, off + charstrings, &cs) < 0)
        return -EINVAL;
    f->cff.charstrings = cs.off;
    f->cff.ncharstrings = cs.count;
    if (priv_off)
        f->cff.subrs = private_subrs(f, off + priv_off, priv_len);
    if (f->cff.fdarray) f->cff.fdarray += off;
    if (f->cff.fdselect) f->cff.fdselect += off;
    f->cff.present = 1;
    return 0;
}

/* Local subrs for a glyph of a CID font, through FDSelect and FDArray. */
static uint32_t cid_subrs(const struct ofont *f, int glyph)
{
    const uint8_t *sel = at(f, f->cff.fdselect, 1);
    if (!sel || !f->cff.fdarray)
        return 0;
    int fd = -1;
    if (sel[0] == 0) {
        const uint8_t *p = at(f, f->cff.fdselect + 1 + (uint32_t)glyph, 1);
        if (p)
            fd = *p;
    } else if (sel[0] == 3) {
        const uint8_t *p = at(f, f->cff.fdselect, 5);
        if (!p)
            return 0;
        int nr = rd16(p + 1);
        for (int i = 0; i < nr; i++) {
            const uint8_t *r = at(f, f->cff.fdselect + 3 + (uint32_t)i * 3, 5);
            if (!r)
                return 0;
            if (glyph >= rd16(r) && glyph < rd16(r + 3)) {
                fd = r[2];
                break;
            }
        }
    }
    if (fd < 0)
        return 0;
    struct index fds;
    uint32_t doff, dlen;
    if (index_read(f, f->cff.fdarray, &fds) < 0 || index_get(f, &fds, fd, &doff, &dlen) < 0)
        return 0;
    struct dict_iter it = { .p = f->data + doff, .end = f->data + doff + dlen };
    int op;
    while (dict_next(&it, &op))
        if (op == 18 && it.n >= 2)
            return private_subrs(f, f->cff.base + (uint32_t)it.operands[1], (uint32_t)it.operands[0]);
    return 0;
}

/* ---- Type 2 charstrings ---- */

struct t2 {
    const struct ofont *f;
    struct font_outline *o;
    int32_t st[48];
    int n;
    int32_t x, y;
    int nstems;
    int width_parsed;
    int open;
    uint32_t gsubrs, subrs;
    int depth;
    int32_t trans[32];
    int ntrans;
};

static int bias(int count)
{
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

static int t2_moveto(struct t2 *t, int32_t x, int32_t y)
{
    int r = 0;
    if (t->open)
        r = outline_close_contour(t->o);
    t->x = x;
    t->y = y;
    t->open = 1;
    return r < 0 ? r : outline_add_point(t->o, x, y, PT_ON);
}

static int t2_lineto(struct t2 *t, int32_t x, int32_t y)
{
    t->x = x;
    t->y = y;
    return outline_add_point(t->o, x, y, PT_ON);
}

static int t2_curveto(struct t2 *t, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3)
{
    int r;
    if ((r = outline_add_point(t->o, x1, y1, PT_CUBIC)) < 0) return r;
    if ((r = outline_add_point(t->o, x2, y2, PT_CUBIC)) < 0) return r;
    t->x = x3;
    t->y = y3;
    return outline_add_point(t->o, x3, y3, PT_ON);
}

/* The first stack clearing operator may carry the width as an extra
 * leading operand; drop it when the count is odd (or off by one). */
static void take_width(struct t2 *t, int even)
{
    if (t->width_parsed)
        return;
    t->width_parsed = 1;
    if ((even && (t->n & 1)) || (!even && t->n > 0 && !even)) {
        memmove(t->st, t->st + 1, (size_t)(t->n - 1) * sizeof t->st[0]);
        t->n--;
    }
}

static int t2_run(struct t2 *t, uint32_t off, uint32_t len);

static int t2_call(struct t2 *t, uint32_t index_off, int global)
{
    if (t->depth > 10 || t->n < 1 || !index_off)
        return -EINVAL;
    struct index ix;
    if (index_read(t->f, index_off, &ix) < 0)
        return -EINVAL;
    int i = t->st[--t->n] + bias(ix.count);
    uint32_t off, len;
    if (index_get(t->f, &ix, i, &off, &len) < 0)
        return -EINVAL;
    t->depth++;
    int r = t2_run(t, off, len);
    t->depth--;
    return r;
    (void)global;
}

static int t2_run(struct t2 *t, uint32_t off, uint32_t len)
{
    const uint8_t *p = t->f->data + off, *end = p + len;
    if (!at(t->f, off, len))
        return -EINVAL;
    while (p < end) {
        int b0 = *p++;
        if (b0 >= 32 || b0 == 28) {
            int32_t v;
            if (b0 == 28) {
                if (p + 2 > end) return -EINVAL;
                v = rds16(p);
                p += 2;
            } else if (b0 <= 246) {
                v = b0 - 139;
            } else if (b0 <= 250) {
                if (p >= end) return -EINVAL;
                v = (b0 - 247) * 256 + *p++ + 108;
            } else if (b0 <= 254) {
                if (p >= end) return -EINVAL;
                v = -(b0 - 251) * 256 - *p++ - 108;
            } else {
                if (p + 4 > end) return -EINVAL;
                v = (int32_t)rd32(p) / 65536;      /* 16.16, integer part */
                p += 4;
            }
            if (t->n < 48)
                t->st[t->n++] = v;
            continue;
        }
        int r = 0;
        int32_t *s = t->st;
        switch (b0) {
        case 1: case 3: case 18: case 23:          /* stems */
            take_width(t, 1);
            t->nstems += t->n / 2;
            t->n = 0;
            break;
        case 19: case 20:                          /* hintmask, cntrmask */
            take_width(t, 1);
            t->nstems += t->n / 2;
            t->n = 0;
            p += (t->nstems + 7) / 8;
            break;
        case 21:                                   /* rmoveto */
            if (t->n > 2) { memmove(s, s + 1, (size_t)(t->n - 1) * sizeof *s); t->n--; }
            t->width_parsed = 1;
            if (t->n >= 2) r = t2_moveto(t, t->x + s[0], t->y + s[1]);
            t->n = 0;
            break;
        case 22:                                   /* hmoveto */
            if (t->n > 1) { memmove(s, s + 1, (size_t)(t->n - 1) * sizeof *s); t->n--; }
            t->width_parsed = 1;
            if (t->n >= 1) r = t2_moveto(t, t->x + s[0], t->y);
            t->n = 0;
            break;
        case 4:                                    /* vmoveto */
            if (t->n > 1) { memmove(s, s + 1, (size_t)(t->n - 1) * sizeof *s); t->n--; }
            t->width_parsed = 1;
            if (t->n >= 1) r = t2_moveto(t, t->x, t->y + s[0]);
            t->n = 0;
            break;
        case 5:                                    /* rlineto */
            for (int i = 0; i + 1 < t->n && r >= 0; i += 2)
                r = t2_lineto(t, t->x + s[i], t->y + s[i + 1]);
            t->n = 0;
            break;
        case 6: case 7: {                          /* hlineto, vlineto */
            int horiz = b0 == 6;
            for (int i = 0; i < t->n && r >= 0; i++, horiz = !horiz)
                r = horiz ? t2_lineto(t, t->x + s[i], t->y) : t2_lineto(t, t->x, t->y + s[i]);
            t->n = 0;
            break;
        }
        case 8:                                    /* rrcurveto */
            for (int i = 0; i + 5 < t->n && r >= 0; i += 6) {
                int32_t x1 = t->x + s[i], y1 = t->y + s[i + 1], x2 = x1 + s[i + 2], y2 = y1 + s[i + 3];
                r = t2_curveto(t, x1, y1, x2, y2, x2 + s[i + 4], y2 + s[i + 5]);
            }
            t->n = 0;
            break;
        case 24:                                   /* rcurveline */
            {
                int i = 0;
                for (; i + 5 < t->n - 2 && r >= 0; i += 6) {
                    int32_t x1 = t->x + s[i], y1 = t->y + s[i + 1], x2 = x1 + s[i + 2], y2 = y1 + s[i + 3];
                    r = t2_curveto(t, x1, y1, x2, y2, x2 + s[i + 4], y2 + s[i + 5]);
                }
                if (i + 1 < t->n && r >= 0)
                    r = t2_lineto(t, t->x + s[i], t->y + s[i + 1]);
            }
            t->n = 0;
            break;
        case 25:                                   /* rlinecurve */
            {
                int i = 0;
                for (; i + 1 < t->n - 6 && r >= 0; i += 2)
                    r = t2_lineto(t, t->x + s[i], t->y + s[i + 1]);
                if (i + 5 < t->n && r >= 0) {
                    int32_t x1 = t->x + s[i], y1 = t->y + s[i + 1], x2 = x1 + s[i + 2], y2 = y1 + s[i + 3];
                    r = t2_curveto(t, x1, y1, x2, y2, x2 + s[i + 4], y2 + s[i + 5]);
                }
            }
            t->n = 0;
            break;
        case 26: case 27: {                        /* vvcurveto, hhcurveto */
            int i = 0;
            int32_t d1 = 0;
            if (t->n & 1)
                d1 = s[i++];
            for (; i + 3 < t->n && r >= 0; i += 4) {
                int32_t x1, y1;
                if (b0 == 26) { x1 = t->x + d1; y1 = t->y + s[i]; }
                else { x1 = t->x + s[i]; y1 = t->y + d1; }
                int32_t x2 = x1 + s[i + 1], y2 = y1 + s[i + 2];
                r = b0 == 26 ? t2_curveto(t, x1, y1, x2, y2, x2, y2 + s[i + 3])
                             : t2_curveto(t, x1, y1, x2, y2, x2 + s[i + 3], y2);
                d1 = 0;
            }
            t->n = 0;
            break;
        }
        case 30: case 31: {                        /* vhcurveto, hvcurveto */
            int horiz = b0 == 31;
            int i = 0;
            while (i + 3 < t->n && r >= 0) {
                int last = i + 8 > t->n;           /* last curve may take a 5th argument */
                int32_t x1, y1, x2, y2, x3, y3;
                if (horiz) {
                    x1 = t->x + s[i]; y1 = t->y;
                    x2 = x1 + s[i + 1]; y2 = y1 + s[i + 2];
                    y3 = y2 + s[i + 3];
                    x3 = x2 + (last && i + 4 < t->n ? s[i + 4] : 0);
                } else {
                    x1 = t->x; y1 = t->y + s[i];
                    x2 = x1 + s[i + 1]; y2 = y1 + s[i + 2];
                    x3 = x2 + s[i + 3];
                    y3 = y2 + (last && i + 4 < t->n ? s[i + 4] : 0);
                }
                r = t2_curveto(t, x1, y1, x2, y2, x3, y3);
                horiz = !horiz;
                i += 4;
            }
            t->n = 0;
            break;
        }
        case 10:                                   /* callsubr */
            r = t2_call(t, t->subrs, 0);
            break;
        case 29:                                   /* callgsubr */
            r = t2_call(t, t->gsubrs, 1);
            break;
        case 11:                                   /* return */
            return 0;
        case 14:                                   /* endchar */
            take_width(t, 0);
            if (t->open)
                r = outline_close_contour(t->o);
            t->open = 0;
            return r;
        case 12: {
            if (p >= end) return -EINVAL;
            int b1 = *p++;
            switch (b1) {
            case 35:                               /* flex */
                if (t->n >= 13) {
                    int32_t x1 = t->x + s[0], y1 = t->y + s[1], x2 = x1 + s[2], y2 = y1 + s[3], jx = x2 + s[4], jy = y2 + s[5];
                    r = t2_curveto(t, x1, y1, x2, y2, jx, jy);
                    int32_t x3 = jx + s[6], y3 = jy + s[7], x4 = x3 + s[8], y4 = y3 + s[9];
                    if (r >= 0) r = t2_curveto(t, x3, y3, x4, y4, x4 + s[10], y4 + s[11]);
                }
                t->n = 0;
                break;
            case 34:                               /* hflex */
                if (t->n >= 7) {
                    int32_t y0 = t->y;
                    int32_t x1 = t->x + s[0], y1 = t->y, x2 = x1 + s[1], y2 = y1 + s[2], jx = x2 + s[3], jy = y2;
                    r = t2_curveto(t, x1, y1, x2, y2, jx, jy);
                    int32_t x3 = jx + s[4], y3 = y2, x4 = x3 + s[5], y4 = y0;
                    if (r >= 0) r = t2_curveto(t, x3, y3, x4, y4, x4 + s[6], y0);
                }
                t->n = 0;
                break;
            case 36:                               /* hflex1 */
                if (t->n >= 9) {
                    int32_t y0 = t->y;
                    int32_t x1 = t->x + s[0], y1 = t->y + s[1], x2 = x1 + s[2], y2 = y1 + s[3], jx = x2 + s[4], jy = y2;
                    r = t2_curveto(t, x1, y1, x2, y2, jx, jy);
                    int32_t x3 = jx + s[5], y3 = y2, x4 = x3 + s[6], y4 = y3 + s[7];
                    if (r >= 0) r = t2_curveto(t, x3, y3, x4, y4, x4 + s[8], y0);
                }
                t->n = 0;
                break;
            case 37:                               /* flex1 */
                if (t->n >= 11) {
                    int32_t sx = t->x, sy = t->y;
                    int32_t dx = 0, dy = 0;
                    for (int i = 0; i < 10; i += 2) { dx += s[i]; dy += s[i + 1]; }
                    int32_t x1 = t->x + s[0], y1 = t->y + s[1], x2 = x1 + s[2], y2 = y1 + s[3], jx = x2 + s[4], jy = y2 + s[5];
                    r = t2_curveto(t, x1, y1, x2, y2, jx, jy);
                    int32_t x3 = jx + s[6], y3 = jy + s[7], x4 = x3 + s[8], y4 = y3 + s[9];
                    int32_t x5, y5;
                    if (abs(dx) > abs(dy)) { x5 = x4 + s[10]; y5 = sy; }
                    else { x5 = sx; y5 = y4 + s[10]; }
                    if (r >= 0) r = t2_curveto(t, x3, y3, x4, y4, x5, y5);
                }
                t->n = 0;
                break;
            default:                               /* arithmetic and others: clear */
                t->n = 0;
                break;
            }
            break;
        }
        default:
            t->n = 0;
            break;
        }
        if (r < 0)
            return r;
    }
    return 0;
}

int cff_outline(const struct ofont *f, int glyph, struct font_outline *o)
{
    if (glyph < 0 || glyph >= f->cff.ncharstrings)
        return -EINVAL;
    struct index cs;
    uint32_t off, len;
    if (index_read(f, f->cff.charstrings, &cs) < 0 || index_get(f, &cs, glyph, &off, &len) < 0)
        return -EINVAL;
    struct t2 t = { .f = f, .o = o, .gsubrs = f->cff.gsubrs, .subrs = f->cff.subrs };
    if (f->cff.is_cid) {
        uint32_t s = cid_subrs(f, glyph);
        if (s)
            t.subrs = s;
    }
    int r = t2_run(&t, off, len);
    if (r >= 0 && t.open)
        r = outline_close_contour(o);
    return r;
}
