/* Font files: table directory, metrics, character map, TrueType glyf
 * outlines including composites. */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* ---- outlines ---- */

int outline_add_point(struct font_outline *o, int32_t x, int32_t y, int kind)
{
    if ((o->npoints & 63) == 0) {
        struct font_point *p = realloc(o->points, (size_t)(o->npoints + 64) * sizeof *p);
        if (!p)
            return -ENOMEM;
        o->points = p;
    }
    o->points[o->npoints].x = x;
    o->points[o->npoints].y = y;
    o->points[o->npoints].on_curve = (uint8_t)kind;
    o->npoints++;
    return 0;
}

int outline_close_contour(struct font_outline *o)
{
    if (o->npoints == 0 || (o->ncontours && o->contour_end[o->ncontours - 1] == o->npoints - 1))
        return 0;
    if ((o->ncontours & 15) == 0) {
        int *e = realloc(o->contour_end, (size_t)(o->ncontours + 16) * sizeof *e);
        if (!e)
            return -ENOMEM;
        o->contour_end = e;
    }
    o->contour_end[o->ncontours++] = o->npoints - 1;
    return 0;
}

void outline_bounds(struct font_outline *o)
{
    o->xmin = o->ymin = 0;
    o->xmax = o->ymax = 0;
    for (int i = 0; i < o->npoints; i++) {
        struct font_point *p = &o->points[i];
        if (i == 0 || p->x < o->xmin) o->xmin = p->x;
        if (i == 0 || p->x > o->xmax) o->xmax = p->x;
        if (i == 0 || p->y < o->ymin) o->ymin = p->y;
        if (i == 0 || p->y > o->ymax) o->ymax = p->y;
    }
}

void font_outline_free(struct font_outline *o)
{
    free(o->points);
    free(o->contour_end);
    memset(o, 0, sizeof *o);
}

/* ---- file and tables ---- */

static int find_table(const struct ofont *f, const char *tag, uint32_t *off, uint32_t *len)
{
    if (f->size < 12)
        return 0;
    int n = rd16(f->data + 4);
    for (int i = 0; i < n; i++) {
        const uint8_t *e = f->data + 12 + i * 16;
        if (e + 16 > f->data + f->size)
            return 0;
        if (memcmp(e, tag, 4) == 0) {
            *off = rd32(e + 8);
            *len = rd32(e + 12);
            return *off < f->size && *len <= f->size - *off;
        }
    }
    return 0;
}

static int pick_cmap(struct ofont *f, uint32_t cmap, uint32_t len)
{
    if (len < 4)
        return -EINVAL;
    int n = rd16(f->data + cmap + 2);
    int best = -1;
    uint32_t best_off = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = f->data + cmap + 4 + i * 8;
        if (e + 8 > f->data + f->size)
            break;
        int plat = rd16(e), enc = rd16(e + 2);
        uint32_t off = rd32(e + 4);
        if (off + 4 > len)
            continue;
        int format = rd16(f->data + cmap + off);
        int score = 0;
        if (plat == 3 && enc == 10 && format == 12) score = 5;
        else if (plat == 0 && format == 12) score = 4;
        else if (plat == 3 && enc == 1 && format == 4) score = 3;
        else if (plat == 0 && format == 4) score = 2;
        else if (format == 0 || format == 6) score = 1;
        if (score > best) {
            best = score;
            best_off = cmap + off;
            f->cmap_format = format;
        }
    }
    if (best < 0)
        return -EINVAL;
    f->cmap_sub = best_off;
    return 0;
}

struct ofont *font_open(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;
    struct ofont *f = calloc(1, sizeof *f);
    if (!f) {
        fclose(fp);
        return NULL;
    }
    size_t cap = 65536, n = 0;
    f->data = malloc(cap);
    for (;;) {
        if (!f->data)
            goto fail;
        size_t got = fread(f->data + n, 1, cap - n, fp);
        n += got;
        if (n < cap)
            break;
        cap *= 2;
        f->data = realloc(f->data, cap);
    }
    fclose(fp);
    fp = NULL;
    f->size = n;
    f->path = strdup(path);
    if (n < 12)
        goto invalid;
    uint32_t magic = rd32(f->data);
    if (magic != 0x00010000 && magic != 0x74727565 && magic != 0x4f54544f)
        goto invalid;
    uint32_t off, len;
    if (!find_table(f, "head", &off, &len) || len < 54)
        goto invalid;
    f->upem = rd16(f->data + off + 18);
    f->long_loca = rds16(f->data + off + 50) != 0;
    if (f->upem <= 0)
        goto invalid;
    if (!find_table(f, "hhea", &off, &len) || len < 36)
        goto invalid;
    f->ascent = rds16(f->data + off + 4);
    f->descent = rds16(f->data + off + 6);
    f->line_gap = rds16(f->data + off + 8);
    f->num_hmetrics = rd16(f->data + off + 34);
    if (!find_table(f, "maxp", &off, &len) || len < 6)
        goto invalid;
    f->nglyphs = rd16(f->data + off + 4);
    if (!find_table(f, "hmtx", &f->hmtx, &f->hmtx_len))
        goto invalid;
    if (!find_table(f, "cmap", &off, &len) || pick_cmap(f, off, len) < 0)
        goto invalid;
    if (find_table(f, "CFF ", &off, &len)) {
        if (cff_parse(f, off, len) < 0)
            goto invalid;
    } else {
        if (!find_table(f, "loca", &f->loca, &f->loca_len) || !find_table(f, "glyf", &f->glyf, &f->glyf_len))
            goto invalid;
    }
    if (!find_table(f, "kern", &f->kern, &f->kern_len))
        f->kern = 0;
    if (!find_table(f, "GPOS", &f->gpos, &f->gpos_len))
        f->gpos = 0;
    return f;
invalid:
    errno = EINVAL;
fail:
    if (fp)
        fclose(fp);
    font_close(f);
    return NULL;
}

void font_close(struct ofont *f)
{
    if (!f)
        return;
    cache_free(f);
    free(f->data);
    free(f->path);
    free(f);
}

const char *font_name(const struct ofont *f) { return f->path; }
int font_units_per_em(const struct ofont *f) { return f->upem; }
int font_glyph_count(const struct ofont *f) { return f->nglyphs; }
int font_is_cff(const struct ofont *f) { return f->cff.present; }

void font_metrics(const struct ofont *f, int *ascent, int *descent, int *line_gap)
{
    if (ascent) *ascent = f->ascent;
    if (descent) *descent = f->descent;
    if (line_gap) *line_gap = f->line_gap;
}

/* ---- character map ---- */

int font_glyph_index(const struct ofont *f, uint32_t cp)
{
    const uint8_t *t = f->data + f->cmap_sub;
    const uint8_t *end = f->data + f->size;
    switch (f->cmap_format) {
    case 0:
        return cp < 256 && t + 6 + cp < end ? t[6 + cp] : 0;
    case 4: {
        if (cp > 0xffff)
            return 0;
        int segx2 = rd16(t + 6);
        const uint8_t *ends = t + 14, *starts = ends + segx2 + 2, *deltas = starts + segx2, *ranges = deltas + segx2;
        if (ranges + segx2 > end)
            return 0;
        for (int s = 0; s < segx2 / 2; s++) {
            unsigned e = rd16(ends + 2 * s);
            if (cp > e)
                continue;
            unsigned st = rd16(starts + 2 * s);
            if (cp < st)
                return 0;
            unsigned delta = rd16(deltas + 2 * s), ro = rd16(ranges + 2 * s);
            if (ro == 0)
                return (int)((cp + delta) & 0xffff);
            const uint8_t *gp = ranges + 2 * s + ro + 2 * (cp - st);
            if (gp + 2 > end)
                return 0;
            unsigned g = rd16(gp);
            return g ? (int)((g + delta) & 0xffff) : 0;
        }
        return 0;
    }
    case 6: {
        unsigned first = rd16(t + 6), count = rd16(t + 8);
        if (cp < first || cp >= first + count || t + 10 + 2 * (cp - first) + 2 > end)
            return 0;
        return rd16(t + 10 + 2 * (cp - first));
    }
    case 12: {
        uint32_t ngroups = rd32(t + 12);
        for (uint32_t g = 0; g < ngroups; g++) {
            const uint8_t *e = t + 16 + g * 12;
            if (e + 12 > end)
                return 0;
            uint32_t st = rd32(e), en = rd32(e + 4);
            if (cp < st)
                return 0;
            if (cp <= en)
                return (int)(rd32(e + 8) + (cp - st));
        }
        return 0;
    }
    }
    return 0;
}

/* ---- horizontal metrics ---- */

int font_advance(const struct ofont *f, int glyph)
{
    if (glyph < 0 || glyph >= f->nglyphs || f->num_hmetrics <= 0)
        return 0;
    int i = glyph < f->num_hmetrics ? glyph : f->num_hmetrics - 1;
    if ((uint32_t)(4 * i + 2) > f->hmtx_len)
        return 0;
    return rd16(f->data + f->hmtx + 4 * i);
}

int font_lsb(const struct ofont *f, int glyph)
{
    if (glyph < 0 || glyph >= f->nglyphs || f->num_hmetrics <= 0)
        return 0;
    uint32_t off = glyph < f->num_hmetrics ? (uint32_t)(4 * glyph + 2)
                                           : (uint32_t)(4 * f->num_hmetrics + 2 * (glyph - f->num_hmetrics));
    if (off + 2 > f->hmtx_len)
        return 0;
    return rds16(f->data + f->hmtx + off);
}

/* ---- glyf outlines ---- */

static int glyph_location(const struct ofont *f, int glyph, uint32_t *off, uint32_t *len)
{
    uint32_t a, b;
    if (f->long_loca) {
        if ((uint32_t)(4 * glyph + 8) > f->loca_len)
            return -EINVAL;
        a = rd32(f->data + f->loca + 4 * glyph);
        b = rd32(f->data + f->loca + 4 * glyph + 4);
    } else {
        if ((uint32_t)(2 * glyph + 4) > f->loca_len)
            return -EINVAL;
        a = rd16(f->data + f->loca + 2 * glyph) * 2u;
        b = rd16(f->data + f->loca + 2 * glyph + 2) * 2u;
    }
    if (b < a || b > f->glyf_len)
        return -EINVAL;
    *off = f->glyf + a;
    *len = b - a;
    return 0;
}

static int simple_glyph(const struct ofont *f, const uint8_t *g, uint32_t len, int ncontours, struct font_outline *o)
{
    const uint8_t *end = g + len;
    const uint8_t *ends = g + 10;
    if (ends + 2 * ncontours + 2 > end)
        return -EINVAL;
    int npts = ncontours ? rd16(ends + 2 * (ncontours - 1)) + 1 : 0;
    if (npts > 4096)
        return -EINVAL;
    int ilen = rd16(ends + 2 * ncontours);
    const uint8_t *p = ends + 2 * ncontours + 2 + ilen;
    uint8_t *flags = malloc((size_t)npts);
    int32_t *xs = malloc((size_t)npts * sizeof *xs), *ys = malloc((size_t)npts * sizeof *ys);
    int r = -EINVAL;
    if (!flags || !xs || !ys) {
        r = -ENOMEM;
        goto out;
    }
    for (int i = 0; i < npts;) {
        if (p >= end)
            goto out;
        uint8_t fl = *p++;
        flags[i++] = fl;
        if (fl & 8) {
            if (p >= end)
                goto out;
            int rep = *p++;
            while (rep-- > 0 && i < npts)
                flags[i++] = fl;
        }
    }
    int32_t v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = flags[i];
        if (fl & 2) {
            if (p >= end) goto out;
            int d = *p++;
            v += (fl & 16) ? d : -d;
        } else if (!(fl & 16)) {
            if (p + 2 > end) goto out;
            v += rds16(p);
            p += 2;
        }
        xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = flags[i];
        if (fl & 4) {
            if (p >= end) goto out;
            int d = *p++;
            v += (fl & 32) ? d : -d;
        } else if (!(fl & 32)) {
            if (p + 2 > end) goto out;
            v += rds16(p);
            p += 2;
        }
        ys[i] = v;
    }
    int start = 0;
    for (int c = 0; c < ncontours; c++) {
        int last = rd16(ends + 2 * c);
        for (int i = start; i <= last && i < npts; i++)
            if ((r = outline_add_point(o, xs[i], ys[i], (flags[i] & 1) ? PT_ON : PT_QUAD)) < 0)
                goto out;
        if ((r = outline_close_contour(o)) < 0)
            goto out;
        start = last + 1;
    }
    r = 0;
out:
    free(flags);
    free(xs);
    free(ys);
    return r;
}

static int composite_glyph(const struct ofont *f, const uint8_t *g, uint32_t len, struct font_outline *o, int depth)
{
    const uint8_t *p = g + 10, *end = g + len;
    for (;;) {
        if (p + 4 > end)
            return -EINVAL;
        int flags = rd16(p), index = rd16(p + 2);
        p += 4;
        int32_t dx, dy;
        if (flags & 1) {
            if (p + 4 > end) return -EINVAL;
            dx = rds16(p);
            dy = rds16(p + 2);
            p += 4;
        } else {
            if (p + 2 > end) return -EINVAL;
            dx = (int8_t)p[0];
            dy = (int8_t)p[1];
            p += 2;
        }
        /* 2.14 transform, identity by default. */
        int32_t a = 16384, b = 0, c = 0, d = 16384;
        if (flags & 8) {
            if (p + 2 > end) return -EINVAL;
            a = d = rds16(p);
            p += 2;
        } else if (flags & 0x40) {
            if (p + 4 > end) return -EINVAL;
            a = rds16(p);
            d = rds16(p + 2);
            p += 4;
        } else if (flags & 0x80) {
            if (p + 8 > end) return -EINVAL;
            a = rds16(p);
            b = rds16(p + 2);
            c = rds16(p + 4);
            d = rds16(p + 6);
            p += 8;
        }
        if (!(flags & 2)) {
            /* Point matching is not supported; treat as no offset. */
            dx = dy = 0;
        }
        struct font_outline sub = { 0 };
        int r = glyf_outline(f, index, &sub, depth + 1);
        if (r < 0) {
            font_outline_free(&sub);
            return r;
        }
        int start = 0;
        for (int ci = 0; ci < sub.ncontours; ci++) {
            for (int i = start; i <= sub.contour_end[ci]; i++) {
                struct font_point *q = &sub.points[i];
                int32_t x = (int32_t)(((int64_t)a * q->x + (int64_t)c * q->y) / 16384) + dx;
                int32_t y = (int32_t)(((int64_t)b * q->x + (int64_t)d * q->y) / 16384) + dy;
                if ((r = outline_add_point(o, x, y, q->on_curve)) < 0) {
                    font_outline_free(&sub);
                    return r;
                }
            }
            if ((r = outline_close_contour(o)) < 0) {
                font_outline_free(&sub);
                return r;
            }
            start = sub.contour_end[ci] + 1;
        }
        font_outline_free(&sub);
        if (!(flags & 0x20))
            break;
    }
    return 0;
}

int glyf_outline(const struct ofont *f, int glyph, struct font_outline *o, int depth)
{
    if (glyph < 0 || glyph >= f->nglyphs || depth > 8)
        return -EINVAL;
    uint32_t off, len;
    int r = glyph_location(f, glyph, &off, &len);
    if (r < 0)
        return r;
    if (len == 0)
        return 0;
    if (len < 10)
        return -EINVAL;
    const uint8_t *g = f->data + off;
    int ncontours = rds16(g);
    if (ncontours >= 0)
        return simple_glyph(f, g, len, ncontours, o);
    return composite_glyph(f, g, len, o, depth);
}

int font_outline(const struct ofont *f, int glyph, struct font_outline *out)
{
    memset(out, 0, sizeof *out);
    int r = f->cff.present ? cff_outline(f, glyph, out) : glyf_outline(f, glyph, out, 0);
    if (r < 0) {
        font_outline_free(out);
        return r;
    }
    outline_bounds(out);
    return 0;
}
