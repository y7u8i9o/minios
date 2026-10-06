/* Internal interface of the AAC-LC decoder (docs/design/codecs.md).
 *
 * The module is organised in layers:
 *
 * - tables.c contains the normative tables of ISO/IEC 14496-3. The script
 *   tools/gen_aac_tables.py generates it.
 * - huffman.c builds decoding trees for the Huffman codebooks.
 * - config.c parses the AudioSpecificConfig and the program configuration
 *   elements, which describe the stream.
 * - decode.c decodes one raw_data_block (1024 samples per channel) into
 *   spectral coefficients and applies the spectral tools.
 * - filterbank.c converts the coefficients into samples with the IMDCT,
 *   windowing and overlap-add.
 * - container.c locates the frames in ADTS and MP4 files.
 * - module.c connects the decoder to libcodec. */
#pragma once
#include <codec/codec.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define AAC_FRAME 1024                  /* samples per channel in one frame */
#define AAC_SHORT 128                   /* samples per short window */
#define AAC_MAX_CHANNELS 8
#define AAC_MAX_SFB 51                  /* maximum number of scale factor bands in a long window */
#define AAC_MAX_ELEMENTS 16

/* ---- tables (tables.c) ---- */

/* A Huffman codebook. It stores the code of each symbol as 32 or 16 bits,
 * together with the length of the code in bits. */
struct aac_codebook {
    const uint32_t *code32;
    const uint16_t *code16;
    const uint8_t *bits;
    unsigned size;
};

/* The scale factor bands of one sample rate index. The structure contains
 * the band offsets of long and short windows, the number of bands in each
 * and the number of bands that TNS may filter. */
struct aac_bands {
    const uint16_t *long_offsets;
    unsigned long_count;
    const uint16_t *short_offsets;
    unsigned short_count;
    unsigned tns_long, tns_short;
};

extern const struct aac_codebook aac_sf_codebook;
extern const struct aac_codebook aac_spectral_codebooks[11];
extern const struct aac_bands aac_bands[13];

/* ---- bit reading ---- */

/* Reads bits from data, most significant bit first. A read past the end
 * returns zero bits and sets overrun. The caller checks overrun once per
 * group of syntax elements instead of after every read. */
struct aac_bits {
    const uint8_t *data;
    size_t bits;                        /* number of bits in data */
    size_t pos;                         /* position of the next bit to read */
    int overrun;
};

static inline void aac_bits_init(struct aac_bits *b, const uint8_t *data, size_t len)
{
    b->data = data;
    b->bits = len * 8;
    b->pos = 0;
    b->overrun = 0;
}

static inline unsigned aac_bit(struct aac_bits *b)
{
    if (b->pos >= b->bits) {
        b->overrun = 1;
        return 0;
    }
    unsigned v = (b->data[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

/* Reads n bits, where n is at most 32. */
static inline uint32_t aac_get(struct aac_bits *b, unsigned n)
{
    uint32_t v = 0;
    while (n--)
        v = v << 1 | aac_bit(b);
    return v;
}

static inline void aac_skip(struct aac_bits *b, size_t n)
{
    b->pos += n;
    if (b->pos > b->bits) {
        b->pos = b->bits;
        b->overrun = 1;
    }
}

/* Advances to the next byte boundary, counted from start. */
static inline void aac_align(struct aac_bits *b, size_t start)
{
    size_t rem = (b->pos - start) & 7;
    if (rem)
        aac_skip(b, 8 - rem);
}

/* ---- Huffman decoding (huffman.c) ---- */

/* A binary decoding tree. node[i][bit] is the index of the next node, or
 * the symbol plus one, stored as a negative number. */
struct aac_huffman {
    int16_t (*node)[2];
    unsigned count;
};

int aac_huffman_build(struct aac_huffman *h, const struct aac_codebook *cb);
void aac_huffman_free(struct aac_huffman *h);
/* Returns the next symbol, or -1 for an invalid code or the end of the data. */
int aac_huffman_decode(const struct aac_huffman *h, struct aac_bits *b);

/* ---- stream configuration (config.c) ---- */

enum aac_element {
    AAC_SCE = 0,                        /* single channel element */
    AAC_CPE = 1,                        /* channel pair element */
    AAC_CCE = 2,                        /* coupling channel element */
    AAC_LFE = 3,                        /* low frequency enhancement element */
    AAC_DSE = 4,                        /* data stream element */
    AAC_PCE = 5,                        /* program configuration element */
    AAC_FIL = 6,                        /* fill element */
    AAC_END = 7,
};

/* The configuration of a stream. elements lists the channel elements in
 * the order in which they appear in each frame (AAC_SCE, AAC_CPE or
 * AAC_LFE). The decoder numbers its channels in the order of the elements.
 * order maps each WAV channel position to the decoder channel. */
struct aac_config {
    unsigned object_type;               /* 2 for AAC-LC */
    unsigned rate_index;                /* 0 to 12 */
    unsigned rate;
    unsigned channel_config;            /* 0 means a program configuration element */
    unsigned channels;
    int sbr;                            /* SBR signalled (HE-AAC); ignored */
    unsigned nelements;
    uint8_t elements[AAC_MAX_ELEMENTS];
    uint8_t order[AAC_MAX_CHANNELS];
};

/* Parses an AudioSpecificConfig (section 1.6.2.1 of ISO/IEC 14496-3).
 * Returns -ENOTSUP for a valid configuration that the decoder does not
 * support and -EINVAL for an invalid one. */
int aac_parse_config(const uint8_t *data, size_t len, struct aac_config *cfg);
/* Completes cfg when the object type, rate index and channel configuration
 * are already set, for example from an ADTS header. */
int aac_config_finish(struct aac_config *cfg);
/* Parses a program_config_element that follows its element identifier and
 * stores its channel elements in cfg. Byte alignment is counted from the
 * position start. */
int aac_parse_pce(struct aac_bits *b, size_t start, struct aac_config *cfg);

/* ---- decoding (decode.c, filterbank.c) ---- */

enum aac_window_sequence {
    AAC_ONLY_LONG = 0,
    AAC_LONG_START = 1,
    AAC_EIGHT_SHORT = 2,
    AAC_LONG_STOP = 3,
};

/* The state of one channel that carries over from frame to frame. It
 * consists of the second half of the previous IMDCT output and the window
 * shape of the previous frame. */
struct aac_channel {
    float overlap[AAC_FRAME];
    unsigned prev_shape;
};

struct aac_filterbank {
    struct codec_mdct long_mdct, short_mdct;
    float sine_long[AAC_FRAME], kbd_long[AAC_FRAME];        /* rising halves */
    float sine_short[AAC_SHORT], kbd_short[AAC_SHORT];
    float time[2 * AAC_FRAME];                              /* work space */
    float block[2 * AAC_SHORT];
};

int aac_filterbank_init(struct aac_filterbank *f);
void aac_filterbank_free(struct aac_filterbank *f);
/* Converts the 1024 coefficients in spec into 1024 samples in out. The
 * function uses and updates the overlap of ch. Short windows store their
 * coefficients as eight consecutive blocks of 128. The samples are in the
 * range of 16-bit PCM. */
void aac_filterbank_apply(struct aac_filterbank *f, struct aac_channel *ch, const float *spec, unsigned sequence,
                          unsigned shape, float *out);

struct aac_decoder;

struct aac_decoder *aac_decoder_new(const struct aac_config *cfg);
void aac_decoder_free(struct aac_decoder *d);
/* Decodes one raw_data_block from b into AAC_FRAME frames of interleaved
 * samples in WAV order, scaled to the range -1 to 1. The block ends with
 * its END element. Afterwards b is aligned to the next byte, counted from
 * start. Returns 0 or a negative errno value. */
int aac_decode_block(struct aac_decoder *d, struct aac_bits *b, size_t start, float *out);

/* ---- containers (container.c) ---- */

/* The frames of a file. For ADTS, the stream follows the headers from one
 * frame to the next. For MP4, the frames come from the sample table of the
 * first audio track. */
struct aac_stream {
    struct aac_config cfg;
    const uint8_t *data;
    size_t len;
    int adts;
    /* ADTS: offset of the next header. */
    size_t next;
    /* MP4: offset and size of every sample. */
    uint64_t *offsets;
    uint32_t *sizes;
    uint32_t count, index;
};

/* Opens data as an ADTS or MP4 file. Returns -EINVAL when data is neither
 * and -ENOTSUP for an unsupported stream. */
int aac_stream_open(struct aac_stream *s, const uint8_t *data, size_t len);
void aac_stream_close(struct aac_stream *s);
/* Returns the next frame: the raw data blocks in *frame and *len, and their
 * number in *blocks. The return value is 1 for a frame, 0 at the end of
 * the stream, or -EBADMSG. */
int aac_stream_next(struct aac_stream *s, const uint8_t **frame, size_t *len, unsigned *blocks);
/* Returns the number of samples per channel that the file decodes to, or -1. */
long aac_stream_frames(const struct aac_stream *s);
/* Probe scores of the two containers. */
int aac_adts_probe(const uint8_t *data, size_t len);
int aac_mp4_probe(const uint8_t *data, size_t len);
