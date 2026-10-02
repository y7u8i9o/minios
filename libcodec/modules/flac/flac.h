#pragma once
/* The FLAC module (RFC 9639): decode.c reads native FLAC streams, encode.c
 * writes them, and module.c holds the codec table. This header declares
 * the bit reader, the bit writer, the two CRCs and the code tables used
 * by both directions. */
#include <codec/codec.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FLAC_MAX_CHANNELS 8
#define FLAC_MAX_BLOCK 65535
#define FLAC_MAX_ORDER 32
#define FLAC_STREAMINFO_LEN 34

/* Channel assignments 8 to 10 of the frame header. */
#define FLAC_LEFT_SIDE 8
#define FLAC_SIDE_RIGHT 9
#define FLAC_MID_SIDE 10

struct flac_streaminfo {
    unsigned min_block, max_block;
    uint32_t min_frame, max_frame;
    unsigned rate, channels, bits;
    uint64_t total;                     /* samples per channel, 0 when unknown */
    uint8_t md5[16];
};

/* The bit reader reads most significant bits first from a byte buffer.
 * A read past the end sets failed and returns zeros. */
struct flac_reader {
    const uint8_t *p;
    size_t len;                         /* bytes */
    size_t pos;                         /* bits */
    int failed;
};

uint64_t flac_bits(struct flac_reader *r, unsigned n);          /* n up to 57 */
int64_t flac_sbits(struct flac_reader *r, unsigned n);          /* two's complement, n up to 57 */
uint32_t flac_unary(struct flac_reader *r);                     /* zeros before the next one */
void flac_align(struct flac_reader *r);

/* The bit writer appends most significant bits first to a growing byte
 * buffer. A failed allocation sets failed. */
struct flac_writer {
    uint8_t *data;
    size_t len, cap;                    /* whole bytes written, allocated bytes */
    uint64_t acc;
    unsigned nacc;                      /* bits waiting in acc */
    int failed;
};

void flac_put(struct flac_writer *w, uint64_t v, unsigned n);  /* n up to 32 */
void flac_put_signed(struct flac_writer *w, int64_t v, unsigned n);
void flac_put_unary(struct flac_writer *w, uint32_t zeros);
void flac_put_align(struct flac_writer *w);

uint8_t flac_crc8(const uint8_t *p, size_t n);
uint16_t flac_crc16(const uint8_t *p, size_t n);

/* The block size and sample rate tables of the frame header, indexed by
 * their codes. 0 marks a code with extra bits or no fixed value. */
extern const unsigned flac_block_sizes[16];
extern const unsigned flac_rates[16];
extern const unsigned flac_sample_sizes[8];

int flac_parse_streaminfo(const uint8_t *p, struct flac_streaminfo *si);
/* The bytes of an ID3v2 tag at the start of data, or 0. */
size_t flac_id3_length(const uint8_t *data, size_t len);

/* The MD5 sum of FLAC covers each sample as a signed little endian
 * integer of (bits + 7) / 8 bytes, interleaved by channel. */
void flac_md5_samples(struct codec_md5 *m, const int64_t *const *ch, unsigned channels, unsigned n,
                      unsigned bits);

int flac_open(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames, void **state);
long flac_read(void *state, int32_t *out, long frames);
void flac_close(void *state);
long flac_encode(const struct codec_audio_format *fmt, const int32_t *samples, long frames, uint8_t **result);
