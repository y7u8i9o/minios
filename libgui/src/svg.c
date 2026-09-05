/* A subset of SVG for icons: the viewBox of the <svg> element and its
 * <path> elements (d, fill, fill-rule, fill-opacity, opacity) are
 * rendered into an RGBA image of px by px pixels with antialiasing.
 * Path data supports M L H V C S Q T A Z and their relative forms;
 * curves are flattened, edges are scan converted with four sub rows per
 * pixel and exact horizontal coverage (the method of libfont's
 * rasterizer). Font Awesome's icons use nothing beyond this subset. */
#include <gui/image.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>

#define SUBROWS 4

struct edge {
    int32_t x0, y0, x1, y1;     /* 26.6 pixels, y0 < y1 */
    int dir;
};

struct edges {
    struct edge *e;
    int n, cap;
};

struct canvas {
    int px;
    float sx, sy, ox, oy;       /* view box to pixels */
    uint32_t *pixels;
};

struct path {
    struct edges es;
    const struct canvas *cv;
    float cx, cy, sx, sy;       /* current point, subpath start */
    float qx, qy;               /* last control point for S and T */
    char last;
};

/* ---- edges and rasterization ---- */

static int add_edge(struct edges *es, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (y0 == y1)
        return 0;
    if (es->n == es->cap) {
        int cap = es->cap ? es->cap * 2 : 256;
        struct edge *e = realloc(es->e, (size_t)cap * sizeof *e);
        if (!e)
            return -ENOMEM;
        es->e = e;
        es->cap = cap;
    }
    struct edge *e = &es->e[es->n++];
    if (y0 < y1)
        *e = (struct edge){ x0, y0, x1, y1, 1 };
    else
        *e = (struct edge){ x1, y1, x0, y0, -1 };
    return 0;
}

static int32_t fix(float v) { return (int32_t)floorf(v * 64.0f + 0.5f); }

/* A line in view box coordinates. */
static int line_to(struct path *p, float x, float y)
{
    const struct canvas *c = p->cv;
    int r = add_edge(&p->es, fix((p->cx * c->sx) + c->ox), fix((p->cy * c->sy) + c->oy), fix((x * c->sx) + c->ox),
                     fix((y * c->sy) + c->oy));
    p->cx = x;
    p->cy = y;
    return r;
}

static void span(int32_t *row, int width, int32_t xa, int32_t xb)
{
    if (xa < 0) xa = 0;
    if (xb > width * 64) xb = width * 64;
    if (xb <= xa)
        return;
    int ca = xa >> 6, cb = (xb - 1) >> 6;
    if (ca == cb) {
        row[ca] += xb - xa;
        return;
    }
    row[ca] += 64 - (xa & 63);
    for (int c = ca + 1; c < cb; c++)
        row[c] += 64;
    row[cb] += ((xb - 1) & 63) + 1;
}

struct crossing {
    int32_t x;
    int dir;
};

static uint32_t over(uint32_t dst, uint32_t color, unsigned a)
{
    unsigned da = dst >> 24;
    if (a == 255 || da == 0)
        return (uint32_t)a << 24 | (color & 0x00ffffff);
    unsigned oa = a + da * (255 - a) / 255;
    uint32_t out = (uint32_t)oa << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        unsigned s = (color >> shift) & 255, d = (dst >> shift) & 255;
        unsigned v = (s * a + d * da * (255 - a) / 255) / (oa ? oa : 1);
        out |= (v > 255 ? 255 : v) << shift;
    }
    return out;
}

/* Fill the edges into the canvas with color and opacity (0..255). */
static int fill_edges(struct edges *es, struct canvas *cv, uint32_t color, unsigned opacity, int evenodd)
{
    int width = cv->px, height = cv->px;
    int32_t *row = malloc((size_t)width * sizeof *row);
    struct crossing *cr = malloc((size_t)(es->n ? es->n : 1) * sizeof *cr);
    if (!row || !cr) {
        free(row);
        free(cr);
        return -ENOMEM;
    }
    for (int py = 0; py < height; py++) {
        memset(row, 0, (size_t)width * sizeof *row);
        int any = 0;
        for (int s = 0; s < SUBROWS; s++) {
            int32_t sy = py * 64 + (64 * s + 32) / SUBROWS;
            int nc = 0;
            for (int i = 0; i < es->n; i++) {
                const struct edge *e = &es->e[i];
                if (sy < e->y0 || sy >= e->y1)
                    continue;
                int64_t x = e->x0 + ((int64_t)(e->x1 - e->x0) * (sy - e->y0)) / (e->y1 - e->y0);
                int j = nc++;
                while (j > 0 && cr[j - 1].x > (int32_t)x) {
                    cr[j] = cr[j - 1];
                    j--;
                }
                cr[j].x = (int32_t)x;
                cr[j].dir = e->dir;
            }
            int wind = 0;
            for (int i = 0; i + 1 < nc; i++) {
                wind += evenodd ? 1 : cr[i].dir;
                int inside = evenodd ? (wind & 1) : wind != 0;
                if (inside) {
                    span(row, width, cr[i].x, cr[i + 1].x);
                    any = 1;
                }
            }
        }
        if (!any)
            continue;
        uint32_t *out = cv->pixels + (size_t)py * width;
        for (int px = 0; px < width; px++) {
            int32_t v = row[px] * 255 / (64 * SUBROWS);
            if (v <= 0)
                continue;
            unsigned a = (unsigned)(v > 255 ? 255 : v) * opacity / 255;
            if (a)
                out[px] = over(out[px], color, a);
        }
    }
    free(row);
    free(cr);
    return 0;
}

/* ---- path data ---- */

static void skip_sep(const char **p)
{
    while (**p == ' ' || **p == ',' || **p == '\t' || **p == '\n' || **p == '\r')
        (*p)++;
}

static int number(const char **p, float *out)
{
    skip_sep(p);
    const char *s = *p;
    char *end;
    /* strtof accepts "1.5.5" as 1.5 then ".5", which is the SVG rule. */
    float v = strtof(s, &end);
    if (end == s)
        return 0;
    *p = end;
    *out = v;
    return 1;
}

/* Flags of the arc command are single digits, possibly without separators. */
static int flag(const char **p, int *out)
{
    skip_sep(p);
    if (**p != '0' && **p != '1')
        return 0;
    *out = **p - '0';
    (*p)++;
    return 1;
}

static int cubic(struct path *p, float x1, float y1, float x2, float y2, float x, float y)
{
    float x0 = p->cx, y0 = p->cy;
    const struct canvas *c = p->cv;
    float d = fabsf(x - x0) + fabsf(y - y0) + fabsf(x1 - x0) + fabsf(y1 - y0) + fabsf(x2 - x) + fabsf(y2 - y);
    int n = (int)(d * c->sx / 3.0f) + 3;
    if (n > 64) n = 64;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        float bx = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x;
        float by = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y;
        int r = line_to(p, bx, by);
        if (r < 0)
            return r;
    }
    p->qx = x2;
    p->qy = y2;
    return 0;
}

static int quadratic(struct path *p, float x1, float y1, float x, float y)
{
    float x0 = p->cx, y0 = p->cy;
    int r = cubic(p, x0 + 2.0f / 3.0f * (x1 - x0), y0 + 2.0f / 3.0f * (y1 - y0), x + 2.0f / 3.0f * (x1 - x),
                  y + 2.0f / 3.0f * (y1 - y), x, y);
    p->qx = x1;
    p->qy = y1;
    return r;
}

/* Elliptical arc by the endpoint to centre conversion of the SVG
 * specification, flattened into line segments. */
static int arc(struct path *p, float rx, float ry, float rot, int large, int sweep, float x, float y)
{
    float x0 = p->cx, y0 = p->cy;
    if (rx == 0 || ry == 0 || (x0 == x && y0 == y))
        return line_to(p, x, y);
    rx = fabsf(rx);
    ry = fabsf(ry);
    float phi = rot * 3.14159265f / 180.0f, cph = cosf(phi), sph = sinf(phi);
    float dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    float x1 = cph * dx + sph * dy, y1 = -sph * dx + cph * dy;
    float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) {
        float s = sqrtf(lambda);
        rx *= s;
        ry *= s;
    }
    float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float k = den > 0 && num > 0 ? sqrtf(num / den) : 0;
    if (large == sweep)
        k = -k;
    float cxp = k * rx * y1 / ry, cyp = -k * ry * x1 / rx;
    float cx = cph * cxp - sph * cyp + (x0 + x) / 2, cy = sph * cxp + cph * cyp + (y0 + y) / 2;
    float ux = (x1 - cxp) / rx, uy = (y1 - cyp) / ry, vx = (-x1 - cxp) / rx, vy = (-y1 - cyp) / ry;
    float theta = acosf(fmaxf(-1.0f, fminf(1.0f, ux / sqrtf(ux * ux + uy * uy))));
    if (uy < 0)
        theta = -theta;
    float dot = ux * vx + uy * vy, len = sqrtf((ux * ux + uy * uy) * (vx * vx + vy * vy));
    float delta = acosf(fmaxf(-1.0f, fminf(1.0f, len > 0 ? dot / len : 1)));
    if (ux * vy - uy * vx < 0)
        delta = -delta;
    if (sweep && delta < 0)
        delta += 2 * 3.14159265f;
    else if (!sweep && delta > 0)
        delta -= 2 * 3.14159265f;
    int n = (int)(fabsf(delta) * (rx + ry) * p->cv->sx / 4.0f) + 4;
    if (n > 96) n = 96;
    for (int i = 1; i <= n; i++) {
        float a = theta + delta * (float)i / (float)n;
        float ex = rx * cosf(a), ey = ry * sinf(a);
        int r = line_to(p, cph * ex - sph * ey + cx, sph * ex + cph * ey + cy);
        if (r < 0)
            return r;
    }
    p->cx = x;
    p->cy = y;
    return 0;
}

static int close_path(struct path *p)
{
    int r = 0;
    if (p->cx != p->sx || p->cy != p->sy)
        r = line_to(p, p->sx, p->sy);
    return r;
}

static int parse_path(struct path *p, const char *d)
{
    const char *s = d;
    char cmd = 0;
    float a[7];
    int r = 0;
    p->cx = p->cy = p->sx = p->sy = 0;
    for (;;) {
        skip_sep(&s);
        if (!*s)
            break;
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
            cmd = *s++;
        } else if (!cmd) {
            return -EINVAL;
        } else if (cmd == 'M') {
            cmd = 'L';              /* further pairs after M are lines */
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        int rel = cmd >= 'a' && cmd <= 'z';
        char c = (char)(rel ? cmd - 'a' + 'A' : cmd);
        float ox = rel ? p->cx : 0, oy = rel ? p->cy : 0;
        switch (c) {
        case 'M':
            if (!number(&s, &a[0]) || !number(&s, &a[1])) return -EINVAL;
            r = close_path(p);
            p->cx = p->sx = a[0] + ox;
            p->cy = p->sy = a[1] + oy;
            break;
        case 'L':
            if (!number(&s, &a[0]) || !number(&s, &a[1])) return -EINVAL;
            r = line_to(p, a[0] + ox, a[1] + oy);
            break;
        case 'H':
            if (!number(&s, &a[0])) return -EINVAL;
            r = line_to(p, a[0] + ox, p->cy);
            break;
        case 'V':
            if (!number(&s, &a[0])) return -EINVAL;
            r = line_to(p, p->cx, a[0] + oy);
            break;
        case 'C':
            for (int i = 0; i < 6; i++)
                if (!number(&s, &a[i])) return -EINVAL;
            r = cubic(p, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy, a[4] + ox, a[5] + oy);
            break;
        case 'S': {
            for (int i = 0; i < 4; i++)
                if (!number(&s, &a[i])) return -EINVAL;
            int smooth = p->last == 'C' || p->last == 'S';
            float x1 = smooth ? 2 * p->cx - p->qx : p->cx, y1 = smooth ? 2 * p->cy - p->qy : p->cy;
            r = cubic(p, x1, y1, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
            break;
        }
        case 'Q':
            for (int i = 0; i < 4; i++)
                if (!number(&s, &a[i])) return -EINVAL;
            r = quadratic(p, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
            break;
        case 'T': {
            if (!number(&s, &a[0]) || !number(&s, &a[1])) return -EINVAL;
            int smooth = p->last == 'Q' || p->last == 'T';
            float x1 = smooth ? 2 * p->cx - p->qx : p->cx, y1 = smooth ? 2 * p->cy - p->qy : p->cy;
            r = quadratic(p, x1, y1, a[0] + ox, a[1] + oy);
            break;
        }
        case 'A': {
            int large, sweep;
            if (!number(&s, &a[0]) || !number(&s, &a[1]) || !number(&s, &a[2]) || !flag(&s, &large) ||
                !flag(&s, &sweep) || !number(&s, &a[5]) || !number(&s, &a[6]))
                return -EINVAL;
            r = arc(p, a[0], a[1], a[2], large, sweep, a[5] + ox, a[6] + oy);
            break;
        }
        case 'Z':
            r = close_path(p);
            p->cx = p->sx;
            p->cy = p->sy;
            break;
        default:
            return -EINVAL;
        }
        if (r < 0)
            return r;
        p->last = c;
    }
    return close_path(p);
}

/* ---- the document ---- */

/* Value of attribute name in the tag text [tag, end), NUL terminated
 * into buf; returns 0 when absent. */
static int attribute(const char *tag, const char *end, const char *name, char *buf, size_t size)
{
    size_t nl = strlen(name);
    for (const char *p = tag; p + nl < end; p++) {
        if (strncmp(p, name, nl) != 0 || (p > tag && (p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n')))
            continue;
        const char *q = p + nl;
        while (q < end && (*q == ' ' || *q == '\t'))
            q++;
        if (q >= end || *q != '=')
            continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t'))
            q++;
        if (q >= end || (*q != '"' && *q != '\''))
            continue;
        char quote = *q++;
        const char *v = q;
        while (q < end && *q != quote)
            q++;
        size_t n = (size_t)(q - v);
        if (n >= size)
            n = size - 1;
        memcpy(buf, v, n);
        buf[n] = '\0';
        return 1;
    }
    return 0;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
}

/* "#rgb", "#rrggbb" or a few names; returns 0 for "none". */
static int parse_color(const char *s, uint32_t fallback, uint32_t *out)
{
    if (strcmp(s, "none") == 0 || strcmp(s, "transparent") == 0)
        return 0;
    if (s[0] == '#' && strlen(s) == 4) {
        *out = (uint32_t)(hexval(s[1]) * 17) << 16 | (uint32_t)(hexval(s[2]) * 17) << 8 | (uint32_t)(hexval(s[3]) * 17);
        return 1;
    }
    if (s[0] == '#' && strlen(s) == 7) {
        *out = 0;
        for (int i = 1; i < 7; i++)
            *out = *out << 4 | (uint32_t)hexval(s[i]);
        return 1;
    }
    if (strcmp(s, "black") == 0) { *out = 0; return 1; }
    if (strcmp(s, "white") == 0) { *out = 0xffffff; return 1; }
    *out = fallback;                    /* currentColor and unknown names */
    return 1;
}

struct image *image_render_svg(const char *text, size_t len, int px, uint32_t color)
{
    if (px < 1 || px > 1024)
        return NULL;
    char *doc = malloc(len + 1);
    if (!doc)
        return NULL;
    memcpy(doc, text, len);
    doc[len] = '\0';
    struct canvas cv = { px, 1, 1, 0, 0, calloc((size_t)px * px, 4) };
    if (!cv.pixels) {
        free(doc);
        return NULL;
    }
    int have_box = 0, err = 0;
    char *d = malloc(len + 1), val[64];
    if (!d) {
        free(doc);
        free(cv.pixels);
        return NULL;
    }
    for (const char *p = doc; (p = strchr(p, '<')) != NULL && !err;) {
        if (strncmp(p, "<!--", 4) == 0) {
            const char *e = strstr(p, "-->");
            p = e ? e + 3 : doc + len;
            continue;
        }
        if (p[1] == '?' || p[1] == '!' || p[1] == '/') {
            p = strchr(p, '>') ? strchr(p, '>') + 1 : doc + len;
            continue;
        }
        const char *end = p;
        char quote = 0;
        while (*end && (quote || *end != '>')) {
            if (quote && *end == quote) quote = 0;
            else if (!quote && (*end == '"' || *end == '\'')) quote = *end;
            end++;
        }
        if (strncmp(p, "<svg", 4) == 0 && (p[4] == ' ' || p[4] == '\n' || p[4] == '\t')) {
            float vx = 0, vy = 0, vw = 0, vh = 0;
            if (attribute(p, end, "viewBox", val, sizeof val)) {
                const char *s = val;
                if (!(number(&s, &vx) && number(&s, &vy) && number(&s, &vw) && number(&s, &vh)))
                    vw = 0;
            }
            if (vw <= 0 || vh <= 0) {
                if (attribute(p, end, "width", val, sizeof val)) vw = strtof(val, NULL);
                if (attribute(p, end, "height", val, sizeof val)) vh = strtof(val, NULL);
                vx = vy = 0;
            }
            if (vw > 0 && vh > 0) {
                float s = (float)px / (vw > vh ? vw : vh);
                cv.sx = cv.sy = s;
                cv.ox = ((float)px - vw * s) / 2 - vx * s;
                cv.oy = ((float)px - vh * s) / 2 - vy * s;
                have_box = 1;
            }
        } else if (strncmp(p, "<path", 5) == 0 && (p[5] == ' ' || p[5] == '\n' || p[5] == '\t') && have_box) {
            if (attribute(p, end, "d", d, len + 1)) {
                uint32_t fill = color;
                int visible = 1;
                if (attribute(p, end, "fill", val, sizeof val))
                    visible = parse_color(val, color, &fill);
                unsigned opacity = 255;
                if (attribute(p, end, "opacity", val, sizeof val))
                    opacity = (unsigned)(strtof(val, NULL) * 255.0f + 0.5f);
                if (attribute(p, end, "fill-opacity", val, sizeof val))
                    opacity = opacity * (unsigned)(strtof(val, NULL) * 255.0f + 0.5f) / 255;
                int evenodd = attribute(p, end, "fill-rule", val, sizeof val) && strcmp(val, "evenodd") == 0;
                if (visible && opacity) {
                    struct path path = { { NULL, 0, 0 }, &cv, 0, 0, 0, 0, 0, 0, 0 };
                    err = parse_path(&path, d);
                    if (!err && path.es.n)
                        err = fill_edges(&path.es, &cv, fill, opacity > 255 ? 255 : opacity, evenodd);
                    free(path.es.e);
                }
            }
        }
        p = *end ? end + 1 : end;
    }
    free(d);
    free(doc);
    if (!have_box || err) {
        free(cv.pixels);
        errno = err ? -err : EINVAL;
        return NULL;
    }
    struct image *img = malloc(sizeof *img);
    if (!img) {
        free(cv.pixels);
        return NULL;
    }
    img->w = img->h = px;
    img->pixels = cv.pixels;
    img->scale = 1;
    return img;
}

struct image *image_load_svg(const char *path, int px, uint32_t color)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char *text = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *n = realloc(text, cap);
            if (!n) {
                free(text);
                fclose(f);
                return NULL;
            }
            text = n;
        }
        size_t k = fread(text + len, 1, cap - len, f);
        if (k == 0)
            break;
        len += k;
    }
    fclose(f);
    struct image *img = text ? image_render_svg(text, len, px, color) : NULL;
    free(text);
    return img;
}
