/* Residues (section 8 of the Vorbis I specification). The residue of a
 * channel is the spectrum divided by its floor. Its range from begin to
 * end is cut into partitions, each partition has a classification read
 * from the classbook, and each classification names up to eight books,
 * one per pass, whose vectors are added into the partition. Type 0
 * interleaves the elements of a vector across the partition, type 1
 * stores them in order, and type 2 codes all channels of a submap as one
 * interleaved vector of type 1. */
#include "vorbis.h"

/* Add the vectors of one partition at v[off]. Returns -1 at the end of
 * the packet. */
static int partition(const struct vb_codebook *b, struct vb_reader *r, float *v, unsigned off, unsigned size,
                     unsigned type)
{
    unsigned dims = b->dims;
    if (type == 0) {
        unsigned step = size / dims;
        for (unsigned j = 0; j < step; j++) {
            int e = vb_decode_entry(b, r);
            if (e < 0)
                return -1;
            const float *vec = b->values + (size_t)e * dims;
            for (unsigned k = 0; k < dims; k++)
                v[off + j + k * step] += vec[k];
        }
        return 0;
    }
    for (unsigned i = 0; i < size;) {
        int e = vb_decode_entry(b, r);
        if (e < 0)
            return -1;
        const float *vec = b->values + (size_t)e * dims;
        for (unsigned k = 0; k < dims && i < size; k++)
            v[off + i++] += vec[k];
    }
    return 0;
}

/* Decode the vectors v[0..nch) of length size with the rules of type 0
 * or 1. The end of the packet ends the decode and leaves the rest of the
 * vectors as they are. */
static void decode(const struct vb_setup *s, const struct vb_residue *res, struct vb_reader *r, float **v,
                   const int *skip, unsigned nch, unsigned size, unsigned type)
{
    unsigned begin = res->begin < size ? res->begin : size, end = res->end < size ? res->end : size;
    if (end <= begin)
        return;
    unsigned psize = res->partition_size, parts = (end - begin) / psize;
    const struct vb_codebook *cb = &s->codebooks[res->classbook];
    unsigned per = cb->dims, stride = parts + per;
    if (!parts)
        return;
    uint8_t *cls = malloc((size_t)nch * stride);
    if (!cls)
        return;
    for (unsigned pass = 0; pass < 8; pass++) {
        for (unsigned p = 0; p < parts;) {
            if (pass == 0)
                for (unsigned j = 0; j < nch; j++) {
                    if (skip[j])
                        continue;
                    int e = vb_decode_entry(cb, r);
                    if (e < 0)
                        goto done;
                    unsigned temp = (unsigned)e;
                    for (unsigned i = per; i-- > 0;) {
                        cls[j * stride + p + i] = (uint8_t)(temp % res->classifications);
                        temp /= res->classifications;
                    }
                }
            for (unsigned i = 0; i < per && p < parts; i++, p++)
                for (unsigned j = 0; j < nch; j++) {
                    if (skip[j])
                        continue;
                    int book = res->books[cls[j * stride + p]][pass];
                    if (book >= 0 && partition(&s->codebooks[book], r, v[j], begin + p * psize, psize, type) < 0)
                        goto done;
                }
        }
    }
done:
    free(cls);
}

void vb_residue_decode(const struct vb_setup *s, const struct vb_residue *res, struct vb_reader *r, float **ch,
                       const int *skip, unsigned nch, unsigned n, float *scratch)
{
    unsigned half = n / 2;
    if (res->type != 2) {
        decode(s, res, r, ch, skip, nch, half, res->type);
        return;
    }
    int any = 0;
    for (unsigned j = 0; j < nch; j++)
        any |= !skip[j];
    if (!any)
        return;
    memset(scratch, 0, sizeof *scratch * half * nch);
    int none = 0;
    decode(s, res, r, &scratch, &none, 1, half * nch, 1);
    for (unsigned i = 0; i < half; i++)
        for (unsigned j = 0; j < nch; j++)
            ch[j][i] += scratch[i * nch + j];
}
