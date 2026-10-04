/* Floors (sections 6 and 7 of the Vorbis I specification). A floor is the
 * spectral envelope of a channel: floor 1 as straight lines in the
 * decibel domain between decoded points, floor 0 as the curve of an LSP
 * filter on the Bark scale. The residue is multiplied by it. */
#include "vorbis.h"
#include <math.h>

/* ---- floor 0 ---- */

static double bark(double x)
{
    return 13.1 * atan(0.00074 * x) + 2.24 * atan(0.0000000185 * x * x) + 0.0001 * x;
}

int vb_floor0_maps(struct vb_floor0 *f, const unsigned blocksize[2])
{
    for (int b = 0; b < 2; b++) {
        unsigned n = blocksize[b] / 2;
        f->map[b] = malloc(sizeof *f->map[b] * (n + 1));
        if (!f->map[b])
            return -ENOMEM;
        double scale = f->bark_map_size / bark(0.5 * f->rate);
        for (unsigned i = 0; i < n; i++) {
            int32_t v = (int32_t)floor(bark((double)f->rate * i / (2.0 * n)) * scale);
            f->map[b][i] = v < (int32_t)f->bark_map_size - 1 ? v : (int32_t)f->bark_map_size - 1;
        }
        f->map[b][n] = -1;
    }
    return 0;
}

static int floor0_decode(const struct vb_setup *s, const struct vb_floor0 *f, struct vb_reader *r,
                         struct vb_floor_data *d)
{
    d->amplitude = (int)vb_bits(r, f->amplitude_bits);
    if (r->eop || d->amplitude == 0)
        return 1;
    unsigned book = vb_bits(r, vb_ilog(f->nbooks));
    if (r->eop || book >= f->nbooks)
        return 1;
    const struct vb_codebook *b = &s->codebooks[f->books[book]];
    float last = 0;
    for (unsigned i = 0; i < f->order;) {
        int e = vb_decode_entry(b, r);
        if (e < 0)
            return 1;
        const float *v = b->values + (size_t)e * b->dims;
        for (unsigned k = 0; k < b->dims; k++) {
            if (i < f->order)
                d->coef[i++] = v[k] + last;
            else
                i++;
        }
        last = v[b->dims - 1] + last;
    }
    return 0;
}

static void floor0_render(const struct vb_floor0 *f, const struct vb_floor_data *d, unsigned blockflag, unsigned n,
                          float *out)
{
    const int32_t *map = f->map[blockflag];
    double cos_coef[VB_MAX_ORDER];
    for (unsigned j = 0; j < f->order; j++)
        cos_coef[j] = cos(d->coef[j]);
    double range = (double)((1u << f->amplitude_bits) - 1);
    unsigned half = n / 2;
    for (unsigned i = 0; i < half;) {
        double w = cos(M_PI * map[i] / f->bark_map_size), p, q;
        if (f->order & 1) {
            p = 1 - w * w;
            q = 0.25;
            for (unsigned j = 0; 2 * j + 1 < f->order; j++)
                p *= 4 * (cos_coef[2 * j + 1] - w) * (cos_coef[2 * j + 1] - w);
            for (unsigned j = 0; 2 * j < f->order; j++)
                q *= 4 * (cos_coef[2 * j] - w) * (cos_coef[2 * j] - w);
        } else {
            p = (1 - w) / 2;
            q = (1 + w) / 2;
            for (unsigned j = 0; 2 * j + 1 < f->order; j++) {
                p *= 4 * (cos_coef[2 * j + 1] - w) * (cos_coef[2 * j + 1] - w);
                q *= 4 * (cos_coef[2 * j] - w) * (cos_coef[2 * j] - w);
            }
        }
        double value = exp(0.11512925 * (d->amplitude * f->amplitude_offset / (range * sqrt(p + q)) -
                                         f->amplitude_offset));
        int32_t step = map[i];
        while (i < half && map[i] == step)
            out[i++] = (float)value;
    }
}

/* ---- floor 1 ---- */

static float inverse_db[256];
static int inverse_db_ready;

/* The table of the specification runs geometrically from 1.0649863e-07
 * at 0 to 1.0 at 255, one step of 140 dB / 255. It is computed on first
 * use. Two threads that compute it at the same time write the same
 * values. */
static void inverse_db_init(void)
{
    if (__atomic_load_n(&inverse_db_ready, __ATOMIC_ACQUIRE))
        return;
    double low = log(1.0649863e-07);
    for (int i = 0; i < 256; i++)
        inverse_db[i] = (float)exp(low * (255 - i) / 255.0);
    __atomic_store_n(&inverse_db_ready, 1, __ATOMIC_RELEASE);
}

static const unsigned floor1_ranges[4] = { 256, 128, 86, 64 };

static int floor1_decode(const struct vb_setup *s, const struct vb_floor1 *f, struct vb_reader *r,
                         struct vb_floor_data *d)
{
    if (!vb_bits(r, 1) || r->eop)
        return 1;
    unsigned range = floor1_ranges[f->multiplier - 1], bits = vb_ilog(range - 1);
    d->y[0] = (int)vb_bits(r, bits);
    d->y[1] = (int)vb_bits(r, bits);
    unsigned at = 2;
    for (unsigned i = 0; i < f->partitions; i++) {
        unsigned c = f->partition_class[i], cdim = f->class_dims[c], cbits = f->class_subclasses[c];
        unsigned csub = (1u << cbits) - 1, cval = 0;
        if (cbits) {
            int e = vb_decode_entry(&s->codebooks[f->class_masterbook[c]], r);
            if (e < 0)
                return 1;
            cval = (unsigned)e;
        }
        for (unsigned j = 0; j < cdim; j++) {
            int book = f->subclass_books[c][cval & csub];
            cval >>= cbits;
            if (book >= 0) {
                int e = vb_decode_entry(&s->codebooks[book], r);
                if (e < 0)
                    return 1;
                d->y[at + j] = e;
            } else {
                d->y[at + j] = 0;
            }
        }
        at += cdim;
    }
    return r->eop ? 1 : 0;
}

static int render_point(int x0, int y0, int x1, int y1, int x)
{
    int dy = y1 - y0, adx = x1 - x0, ady = dy < 0 ? -dy : dy;
    int off = ady * (x - x0) / adx;
    return dy < 0 ? y0 - off : y0 + off;
}

static void render_line(int x0, int y0, int x1, int y1, float *v, int n)
{
    int dy = y1 - y0, adx = x1 - x0, ady = dy < 0 ? -dy : dy;
    int base = dy / adx, sy = dy < 0 ? base - 1 : base + 1;
    int abase = base < 0 ? -base : base;
    int x = x0, y = y0, err = 0;
    ady -= abase * adx;
    if (x < n)
        v[x] = inverse_db[y < 0 ? 0 : y > 255 ? 255 : y];
    for (x = x0 + 1; x < x1 && x < n; x++) {
        err += ady;
        if (err >= adx) {
            err -= adx;
            y += sy;
        } else {
            y += base;
        }
        v[x] = inverse_db[y < 0 ? 0 : y > 255 ? 255 : y];
    }
}

static void floor1_render(const struct vb_floor1 *f, const struct vb_floor_data *d, unsigned n, float *out)
{
    inverse_db_init();
    unsigned range = floor1_ranges[f->multiplier - 1];
    int final_y[VB_MAX_FLOOR1_VALUES], used[VB_MAX_FLOOR1_VALUES];
    final_y[0] = d->y[0];
    final_y[1] = d->y[1];
    used[0] = used[1] = 1;
    for (unsigned i = 2; i < f->values; i++) {
        unsigned lo = f->low[i], hi = f->high[i];
        int predicted = render_point(f->x[lo], final_y[lo], f->x[hi], final_y[hi], f->x[i]);
        int val = d->y[i], highroom = (int)range - predicted, lowroom = predicted;
        int room = highroom < lowroom ? highroom * 2 : lowroom * 2;
        if (val) {
            used[lo] = used[hi] = used[i] = 1;
            if (val >= room)
                final_y[i] = highroom > lowroom ? val - lowroom + predicted : predicted - val + highroom - 1;
            else
                final_y[i] = val & 1 ? predicted - (val + 1) / 2 : predicted + val / 2;
        } else {
            used[i] = 0;
            final_y[i] = predicted;
        }
    }
    int half = (int)(n / 2), lx = 0, ly = final_y[f->sorted[0]] * (int)f->multiplier;
    for (unsigned k = 1; k < f->values; k++) {
        unsigned i = f->sorted[k];
        if (!used[i])
            continue;
        int hx = f->x[i], hy = final_y[i] * (int)f->multiplier;
        if (hx > lx)
            render_line(lx, ly, hx, hy, out, half);
        lx = hx;
        ly = hy;
    }
    if (lx < half)
        render_line(lx, ly, half, ly, out, half);
}

/* ---- both ---- */

int vb_floor_decode(const struct vb_setup *s, const struct vb_floor *f, struct vb_reader *r, struct vb_floor_data *d)
{
    return f->type == 0 ? floor0_decode(s, &f->u.f0, r, d) : floor1_decode(s, &f->u.f1, r, d);
}

void vb_floor_render(const struct vb_floor *f, const struct vb_floor_data *d, unsigned blockflag, unsigned n,
                     float *out)
{
    if (f->type == 0)
        floor0_render(&f->u.f0, d, blockflag, n, out);
    else
        floor1_render(&f->u.f1, d, n, out);
}
