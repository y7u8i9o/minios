/* The Vorbis headers: the identification header with the channel count,
 * the rate and the two block sizes, the comment header, and the setup
 * header with the codebooks, floors, residues, mappings and modes. */
#include "vorbis.h"
#include <math.h>

/* ---- bits ---- */

uint32_t vb_bits(struct vb_reader *r, unsigned n)
{
    if (!n)
        return 0;
    if (r->pos + n > r->len * 8) {
        r->eop = 1;
        r->pos = r->len * 8;
        return 0;
    }
    uint32_t v = 0;
    for (unsigned got = 0; got < n;) {
        unsigned off = (unsigned)(r->pos & 7), take = 8 - off;
        if (take > n - got)
            take = n - got;
        uint32_t b = (r->p[r->pos >> 3] >> off) & ((1u << take) - 1);
        v |= b << got;
        got += take;
        r->pos += take;
    }
    return v;
}

/* The packed float of the codebooks: a 21 bit mantissa, a 10 bit exponent
 * with a bias of 788 and a sign bit. */
float vb_float32(uint32_t x)
{
    double mantissa = (double)(x & 0x1fffff);
    int exponent = (int)((x & 0x7fe00000u) >> 21);
    double v = ldexp(mantissa, exponent - 788);
    return (float)(x & 0x80000000u ? -v : v);
}

unsigned vb_ilog(uint32_t x)
{
    unsigned n = 0;
    while (x) {
        n++;
        x >>= 1;
    }
    return n;
}

/* ---- codebooks ---- */

static int tree_insert(struct vb_codebook *b, unsigned *cap, uint32_t code, unsigned len, unsigned entry)
{
    unsigned node = 0;
    for (unsigned i = 0; i < len; i++) {
        unsigned bit = (code >> (len - 1 - i)) & 1, slot = 2 * node + bit;
        if (i == len - 1) {
            if (b->tree[slot])
                return -EINVAL;
            b->tree[slot] = -1 - (int32_t)entry;
            return 0;
        }
        if (b->tree[slot] < 0)
            return -EINVAL;
        if (!b->tree[slot]) {
            if (b->nodes == *cap) {
                unsigned grown = *cap * 2;
                int32_t *t = realloc(b->tree, sizeof *t * 2 * grown);
                if (!t)
                    return -ENOMEM;
                memset(t + 2 * *cap, 0, sizeof *t * 2 * (grown - *cap));
                b->tree = t;
                *cap = grown;
            }
            b->tree[slot] = (int32_t)b->nodes++;
        }
        node = (unsigned)b->tree[slot];
    }
    return 0;
}

/* Assign the codewords in the order of the entries: each entry receives
 * the lowest codeword of its length that is still free (section 3.2.1).
 * available[l] holds the next free codeword of length l, aligned to the
 * top of 32 bits, or 0. */
static int build_tree(struct vb_codebook *b)
{
    unsigned cap = 64;
    b->tree = calloc(2 * cap, sizeof *b->tree);
    if (!b->tree)
        return -ENOMEM;
    b->nodes = 1;
    uint32_t available[33] = { 0 };
    int first = 1;
    for (unsigned e = 0; e < b->entries; e++) {
        unsigned len = b->lengths[e];
        if (!len)
            continue;
        b->used++;
        uint32_t code;
        if (first) {
            first = 0;
            code = 0;
            for (unsigned i = 1; i <= len; i++)
                available[i] = 1u << (32 - i);
        } else {
            unsigned z = len;
            while (z > 0 && !available[z])
                z--;
            if (!z)
                return -EINVAL;         /* more codewords than the lengths allow */
            uint32_t res = available[z];
            available[z] = 0;
            for (unsigned y = len; y > z; y--)
                available[y] = res + (1u << (32 - y));
            code = res >> (32 - len);
        }
        int err = tree_insert(b, &cap, code, len, e);
        if (err)
            return err;
    }
    return 0;
}

int vb_decode_entry(const struct vb_codebook *b, struct vb_reader *r)
{
    unsigned node = 0;
    for (;;) {
        unsigned bit = vb_bits(r, 1);
        if (r->eop)
            return -1;
        int32_t child = b->tree[2 * node + bit];
        if (child < 0)
            return -1 - child;
        if (!child)
            return -1;
        node = (unsigned)child;
    }
}

/* r^dims, or entries + 1 when the power exceeds entries. */
static uint64_t power_capped(unsigned r, unsigned dims, unsigned entries)
{
    uint64_t p = 1;
    for (unsigned i = 0; i < dims; i++) {
        p *= r;
        if (p > entries)
            return (uint64_t)entries + 1;
    }
    return p;
}

/* The largest r with r^dims <= entries. */
static unsigned lookup1_values(unsigned entries, unsigned dims)
{
    unsigned r = (unsigned)floor(pow((double)entries, 1.0 / dims));
    while (power_capped(r + 1, dims, entries) <= entries)
        r++;
    while (r > 0 && power_capped(r, dims, entries) > entries)
        r--;
    return r;
}

static int read_codebook(struct vb_reader *r, struct vb_codebook *b)
{
    if (vb_bits(r, 24) != 0x564342)
        return -EINVAL;
    b->dims = vb_bits(r, 16);
    b->entries = vb_bits(r, 24);
    if (!b->dims || !b->entries || r->eop)
        return -EINVAL;
    b->lengths = calloc(b->entries, 1);
    if (!b->lengths)
        return -ENOMEM;
    if (vb_bits(r, 1)) {
        /* Ordered: runs of entries with increasing lengths. */
        unsigned at = 0, len = vb_bits(r, 5) + 1;
        while (at < b->entries) {
            unsigned count = vb_bits(r, vb_ilog(b->entries - at));
            if (at + count > b->entries || len > 32 || r->eop)
                return -EINVAL;
            memset(b->lengths + at, (int)len, count);
            at += count;
            len++;
        }
    } else {
        int sparse = (int)vb_bits(r, 1);
        for (unsigned e = 0; e < b->entries; e++)
            b->lengths[e] = !sparse || vb_bits(r, 1) ? (uint8_t)(vb_bits(r, 5) + 1) : 0;
    }
    if (r->eop)
        return -EINVAL;
    int err = build_tree(b);
    if (err)
        return err;
    b->lookup = (int)vb_bits(r, 4);
    if (b->lookup == 0)
        return r->eop ? -EINVAL : 0;
    if (b->lookup > 2)
        return -EINVAL;
    float min = vb_float32(vb_bits(r, 32)), delta = vb_float32(vb_bits(r, 32));
    unsigned value_bits = vb_bits(r, 4) + 1, sequence = vb_bits(r, 1);
    uint64_t count = b->lookup == 1 ? lookup1_values(b->entries, b->dims) : (uint64_t)b->entries * b->dims;
    if ((uint64_t)b->entries * b->dims > 1u << 22 || count > 1u << 22 || r->eop)
        return -EINVAL;
    uint32_t *mult = malloc(sizeof *mult * (count ? count : 1));
    b->values = malloc(sizeof *b->values * b->entries * b->dims);
    if (!mult || !b->values) {
        free(mult);
        return -ENOMEM;
    }
    for (uint64_t i = 0; i < count; i++)
        mult[i] = vb_bits(r, value_bits);
    if (r->eop) {
        free(mult);
        return -EINVAL;
    }
    for (unsigned e = 0; e < b->entries; e++) {
        float last = 0;
        unsigned divisor = 1;
        for (unsigned i = 0; i < b->dims; i++) {
            uint64_t off = b->lookup == 1 ? (e / divisor) % count : (uint64_t)e * b->dims + i;
            float v = (float)mult[off] * delta + min + last;
            if (sequence)
                last = v;
            b->values[e * b->dims + i] = v;
            if (b->lookup == 1)
                divisor *= (unsigned)count;
        }
    }
    free(mult);
    return 0;
}

/* ---- headers ---- */

static int is_header(const uint8_t *p, size_t len, int type)
{
    return len >= 7 && p[0] == type && memcmp(p + 1, "vorbis", 6) == 0;
}

int vb_parse_identification(const uint8_t *p, size_t len, struct vb_setup *s)
{
    if (!is_header(p, len, 1) || len < 30)
        return -EINVAL;
    uint32_t version = (uint32_t)p[7] | (uint32_t)p[8] << 8 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 24;
    s->channels = p[11];
    s->rate = (uint32_t)p[12] | (uint32_t)p[13] << 8 | (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
    unsigned b0 = p[28] & 15, b1 = p[28] >> 4;
    if (version != 0 || !s->channels || !s->rate || b0 < 6 || b1 > 13 || b0 > b1 || !(p[29] & 1))
        return -EINVAL;
    s->blocksize[0] = 1u << b0;
    s->blocksize[1] = 1u << b1;
    return 0;
}

int vb_parse_comment(const uint8_t *p, size_t len)
{
    return is_header(p, len, 3) ? 0 : -EINVAL;
}

static int read_floor(struct vb_reader *r, const struct vb_setup *s, struct vb_floor *f)
{
    f->type = (int)vb_bits(r, 16);
    if (f->type == 0) {
        struct vb_floor0 *g = &f->u.f0;
        g->order = vb_bits(r, 8);
        g->rate = vb_bits(r, 16);
        g->bark_map_size = vb_bits(r, 16);
        g->amplitude_bits = vb_bits(r, 6);
        g->amplitude_offset = vb_bits(r, 8);
        g->nbooks = vb_bits(r, 4) + 1;
        for (unsigned i = 0; i < g->nbooks; i++) {
            g->books[i] = (uint8_t)vb_bits(r, 8);
            if (g->books[i] >= s->ncodebooks)
                return -EINVAL;
        }
        if (!g->order || !g->rate || !g->bark_map_size || !g->amplitude_bits)
            return -EINVAL;
        return r->eop ? -EINVAL : 0;
    }
    if (f->type != 1)
        return -EINVAL;
    struct vb_floor1 *g = &f->u.f1;
    g->partitions = vb_bits(r, 5);
    int max_class = -1;
    for (unsigned i = 0; i < g->partitions; i++) {
        g->partition_class[i] = (uint8_t)vb_bits(r, 4);
        if (g->partition_class[i] > max_class)
            max_class = g->partition_class[i];
    }
    g->classes = (unsigned)(max_class + 1);
    for (unsigned c = 0; c < g->classes; c++) {
        g->class_dims[c] = (uint8_t)(vb_bits(r, 3) + 1);
        g->class_subclasses[c] = (uint8_t)vb_bits(r, 2);
        if (g->class_subclasses[c]) {
            g->class_masterbook[c] = (uint8_t)vb_bits(r, 8);
            if (g->class_masterbook[c] >= s->ncodebooks)
                return -EINVAL;
        }
        for (unsigned j = 0; j < 1u << g->class_subclasses[c]; j++) {
            g->subclass_books[c][j] = (int16_t)((int)vb_bits(r, 8) - 1);
            if (g->subclass_books[c][j] >= (int)s->ncodebooks)
                return -EINVAL;
        }
    }
    g->multiplier = vb_bits(r, 2) + 1;
    unsigned rangebits = vb_bits(r, 4);
    g->x[0] = 0;
    g->x[1] = (uint16_t)(1u << rangebits);
    g->values = 2;
    for (unsigned i = 0; i < g->partitions; i++) {
        unsigned c = g->partition_class[i];
        for (unsigned j = 0; j < g->class_dims[c]; j++) {
            if (g->values == VB_MAX_FLOOR1_VALUES)
                return -EINVAL;
            g->x[g->values++] = (uint16_t)vb_bits(r, rangebits);
        }
    }
    if (r->eop)
        return -EINVAL;
    /* The values sorted by x, and for each value from the third on its
     * neighbours among the values before it. */
    for (unsigned i = 0; i < g->values; i++)
        g->sorted[i] = (uint8_t)i;
    for (unsigned i = 1; i < g->values; i++)
        for (unsigned j = i; j > 0 && g->x[g->sorted[j - 1]] > g->x[g->sorted[j]]; j--) {
            uint8_t t = g->sorted[j];
            g->sorted[j] = g->sorted[j - 1];
            g->sorted[j - 1] = t;
        }
    for (unsigned i = 1; i < g->values; i++)
        if (g->x[g->sorted[i]] == g->x[g->sorted[i - 1]])
            return -EINVAL;
    for (unsigned i = 2; i < g->values; i++) {
        int lo = -1, hi = -1;
        for (unsigned j = 0; j < i; j++) {
            if (g->x[j] < g->x[i] && (lo < 0 || g->x[j] > g->x[lo]))
                lo = (int)j;
            if (g->x[j] > g->x[i] && (hi < 0 || g->x[j] < g->x[hi]))
                hi = (int)j;
        }
        g->low[i] = (uint8_t)lo;
        g->high[i] = (uint8_t)hi;
    }
    return 0;
}

static int read_residue(struct vb_reader *r, const struct vb_setup *s, struct vb_residue *res)
{
    res->type = vb_bits(r, 16);
    if (res->type > 2)
        return -EINVAL;
    res->begin = vb_bits(r, 24);
    res->end = vb_bits(r, 24);
    res->partition_size = vb_bits(r, 24) + 1;
    res->classifications = vb_bits(r, 6) + 1;
    res->classbook = vb_bits(r, 8);
    if (res->classbook >= s->ncodebooks)
        return -EINVAL;
    unsigned cascade[64];
    for (unsigned c = 0; c < res->classifications; c++) {
        unsigned low = vb_bits(r, 3), high = vb_bits(r, 1) ? vb_bits(r, 5) : 0;
        cascade[c] = high << 3 | low;
    }
    for (unsigned c = 0; c < res->classifications; c++)
        for (unsigned pass = 0; pass < 8; pass++) {
            res->books[c][pass] = -1;
            if (cascade[c] & (1u << pass)) {
                unsigned book = vb_bits(r, 8);
                if (book >= s->ncodebooks || !s->codebooks[book].values)
                    return -EINVAL;
                res->books[c][pass] = (int16_t)book;
            }
        }
    return r->eop ? -EINVAL : 0;
}

static int read_mapping(struct vb_reader *r, const struct vb_setup *s, struct vb_mapping *m)
{
    if (vb_bits(r, 16) != 0)
        return -EINVAL;
    m->submaps = vb_bits(r, 1) ? vb_bits(r, 4) + 1 : 1;
    m->coupling_steps = vb_bits(r, 1) ? vb_bits(r, 8) + 1 : 0;
    unsigned bits = vb_ilog(s->channels - 1);
    for (unsigned i = 0; i < m->coupling_steps; i++) {
        m->magnitude[i] = (uint8_t)vb_bits(r, bits);
        m->angle[i] = (uint8_t)vb_bits(r, bits);
        if (m->magnitude[i] == m->angle[i] || m->magnitude[i] >= s->channels || m->angle[i] >= s->channels)
            return -EINVAL;
    }
    if (vb_bits(r, 2))
        return -EINVAL;
    for (unsigned c = 0; c < s->channels; c++) {
        m->mux[c] = (uint8_t)(m->submaps > 1 ? vb_bits(r, 4) : 0);
        if (m->mux[c] >= m->submaps)
            return -EINVAL;
    }
    for (unsigned i = 0; i < m->submaps; i++) {
        vb_bits(r, 8);
        m->submap_floor[i] = (uint8_t)vb_bits(r, 8);
        m->submap_residue[i] = (uint8_t)vb_bits(r, 8);
        if (m->submap_floor[i] >= s->nfloors || m->submap_residue[i] >= s->nresidues)
            return -EINVAL;
    }
    return r->eop ? -EINVAL : 0;
}

int vb_parse_setup(const uint8_t *p, size_t len, struct vb_setup *s)
{
    if (!is_header(p, len, 5))
        return -EINVAL;
    struct vb_reader r = { p, len, 7 * 8, 0 };
    s->ncodebooks = vb_bits(&r, 8) + 1;
    s->codebooks = calloc(s->ncodebooks, sizeof *s->codebooks);
    if (!s->codebooks)
        return -ENOMEM;
    for (unsigned i = 0; i < s->ncodebooks; i++) {
        int err = read_codebook(&r, &s->codebooks[i]);
        if (err)
            return err;
    }
    unsigned times = vb_bits(&r, 6) + 1;
    for (unsigned i = 0; i < times; i++)
        if (vb_bits(&r, 16) != 0)
            return -EINVAL;
    s->nfloors = vb_bits(&r, 6) + 1;
    s->floors = calloc(s->nfloors, sizeof *s->floors);
    if (!s->floors)
        return -ENOMEM;
    for (unsigned i = 0; i < s->nfloors; i++) {
        int err = read_floor(&r, s, &s->floors[i]);
        if (err)
            return err;
    }
    s->nresidues = vb_bits(&r, 6) + 1;
    s->residues = calloc(s->nresidues, sizeof *s->residues);
    if (!s->residues)
        return -ENOMEM;
    for (unsigned i = 0; i < s->nresidues; i++) {
        int err = read_residue(&r, s, &s->residues[i]);
        if (err)
            return err;
    }
    s->nmappings = vb_bits(&r, 6) + 1;
    s->mappings = calloc(s->nmappings, sizeof *s->mappings);
    if (!s->mappings)
        return -ENOMEM;
    for (unsigned i = 0; i < s->nmappings; i++) {
        int err = read_mapping(&r, s, &s->mappings[i]);
        if (err)
            return err;
    }
    s->nmodes = vb_bits(&r, 6) + 1;
    for (unsigned i = 0; i < s->nmodes; i++) {
        s->modes[i].blockflag = vb_bits(&r, 1);
        unsigned window = vb_bits(&r, 16), transform = vb_bits(&r, 16);
        s->modes[i].mapping = vb_bits(&r, 8);
        if (window || transform || s->modes[i].mapping >= s->nmappings)
            return -EINVAL;
    }
    if (!vb_bits(&r, 1) || r.eop)
        return -EINVAL;
    /* Floor 0 needs vector books and its Bark maps. */
    for (unsigned i = 0; i < s->nfloors; i++)
        if (s->floors[i].type == 0) {
            for (unsigned j = 0; j < s->floors[i].u.f0.nbooks; j++)
                if (!s->codebooks[s->floors[i].u.f0.books[j]].values)
                    return -EINVAL;
            int err = vb_floor0_maps(&s->floors[i].u.f0, s->blocksize);
            if (err)
                return err;
        }
    return 0;
}

void vb_setup_free(struct vb_setup *s)
{
    for (unsigned i = 0; s->codebooks && i < s->ncodebooks; i++) {
        free(s->codebooks[i].lengths);
        free(s->codebooks[i].tree);
        free(s->codebooks[i].values);
    }
    free(s->codebooks);
    for (unsigned i = 0; s->floors && i < s->nfloors; i++)
        if (s->floors[i].type == 0) {
            free(s->floors[i].u.f0.map[0]);
            free(s->floors[i].u.f0.map[1]);
        }
    free(s->floors);
    free(s->residues);
    free(s->mappings);
    memset(s, 0, sizeof *s);
}
