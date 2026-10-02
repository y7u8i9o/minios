#pragma once
/* The Vorbis module (Vorbis I specification): setup.c reads the three
 * header packets, floor.c, residue.c and mdct.c implement the stages of
 * the audio decode, and decode.c joins them into packets and streams. */
#include <codec/codec.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VB_MAX_CHANNELS 255
#define VB_MAX_FLOOR1_VALUES 65
#define VB_MAX_ORDER 255                /* of floor 0 */

/* The bit reader of Vorbis reads the least significant bits of each byte
 * first. Reading past the end of a packet sets eop and returns zeros. */
struct vb_reader {
    const uint8_t *p;
    size_t len;
    size_t pos;                         /* bits */
    int eop;
};

uint32_t vb_bits(struct vb_reader *r, unsigned n);  /* n up to 32 */
float vb_float32(uint32_t x);
unsigned vb_ilog(uint32_t x);

struct vb_codebook {
    unsigned dims, entries;
    uint8_t *lengths;                   /* per entry, 0 for an unused entry */
    /* The decoding tree: node i has children tree[2i] and tree[2i+1]. A
     * child of 0 is absent, a positive child is a node, and a negative
     * child -1 - e is the leaf of entry e. */
    int32_t *tree;
    unsigned nodes;
    unsigned used;                      /* entries with a codeword */
    int lookup;                         /* 0, 1 or 2 */
    float *values;                      /* entries * dims vector values, or NULL */
};

struct vb_floor0 {
    unsigned order, rate, bark_map_size, amplitude_bits, amplitude_offset, nbooks;
    uint8_t books[16];
    int32_t *map[2];                    /* the Bark map for each block size, computed at setup */
};

struct vb_floor1 {
    unsigned partitions, classes, multiplier, values;
    uint8_t partition_class[32];
    uint8_t class_dims[16], class_subclasses[16], class_masterbook[16];
    int16_t subclass_books[16][8];
    uint16_t x[VB_MAX_FLOOR1_VALUES];
    /* The values in the order of x, and the neighbours of each value. */
    uint8_t sorted[VB_MAX_FLOOR1_VALUES];
    uint8_t low[VB_MAX_FLOOR1_VALUES], high[VB_MAX_FLOOR1_VALUES];
};

struct vb_floor {
    int type;
    union {
        struct vb_floor0 f0;
        struct vb_floor1 f1;
    } u;
};

struct vb_residue {
    unsigned type, begin, end, partition_size, classifications, classbook;
    int16_t books[64][8];
};

struct vb_mapping {
    unsigned submaps, coupling_steps;
    uint8_t magnitude[256], angle[256];
    uint8_t mux[VB_MAX_CHANNELS];
    uint8_t submap_floor[16], submap_residue[16];
};

struct vb_mode {
    unsigned blockflag, mapping;
};

struct vb_setup {
    unsigned channels;
    uint32_t rate;
    unsigned blocksize[2];
    unsigned ncodebooks, nfloors, nresidues, nmappings, nmodes;
    struct vb_codebook *codebooks;
    struct vb_floor *floors;
    struct vb_residue *residues;
    struct vb_mapping *mappings;
    struct vb_mode modes[64];
};

int vb_parse_identification(const uint8_t *p, size_t len, struct vb_setup *s);
int vb_parse_comment(const uint8_t *p, size_t len);
int vb_parse_setup(const uint8_t *p, size_t len, struct vb_setup *s);
void vb_setup_free(struct vb_setup *s);

/* Decode one entry of a codebook, or -1 at the end of the packet or for a
 * codeword that the book does not contain. */
int vb_decode_entry(const struct vb_codebook *b, struct vb_reader *r);

/* Floors: decode the data of one channel, then render the curve into
 * out[0..n/2). decode returns 0 for a floor in use and 1 for an unused
 * floor. */
struct vb_floor_data {
    int amplitude;                      /* floor 0 */
    float coef[VB_MAX_ORDER + 1];
    int y[VB_MAX_FLOOR1_VALUES];        /* floor 1 */
};
int vb_floor_decode(const struct vb_setup *s, const struct vb_floor *f, struct vb_reader *r, struct vb_floor_data *d);
/* Render the curve of a floor for a block with blockflag, whose size is
 * n, into out[0..n/2). */
void vb_floor_render(const struct vb_floor *f, const struct vb_floor_data *d, unsigned blockflag, unsigned n,
                     float *out);
int vb_floor0_maps(struct vb_floor0 *f, const unsigned blocksize[2]);

/* Decode the residue vectors of the channels in ch[0..nch), each n/2
 * values, which the caller has cleared. Channels with skip set are left
 * as they are. */
void vb_residue_decode(const struct vb_setup *s, const struct vb_residue *res, struct vb_reader *r, float **ch,
                       const int *skip, unsigned nch, unsigned n, float *scratch);

/* The inverse MDCT of n/2 coefficients into n samples. */
struct vb_mdct {
    unsigned n;
    float *twiddle, *post;              /* the pre and post rotations */
    float *fft_cos, *fft_sin;
    unsigned *bitrev;
    float *work;
};
int vb_mdct_init(struct vb_mdct *m, unsigned n);
void vb_mdct_free(struct vb_mdct *m);
void vb_imdct(const struct vb_mdct *m, const float *in, float *out);
void vb_imdct_direct(unsigned n, const float *in, float *out);  /* the definition, for the tests */
