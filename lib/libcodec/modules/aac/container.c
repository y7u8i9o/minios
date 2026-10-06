/* The two containers of AAC-LC that the module reads:
 *
 * - ADTS (section 1.A.3 of ISO/IEC 14496-3) is the format of .aac files. It
 *   is a sequence of frames. Each frame has a 7-byte header (9 bytes with a
 *   CRC) that repeats the stream configuration. An ID3v2 tag may precede
 *   the first frame.
 * - MP4 (ISO/IEC 14496-12 and 14496-14) is the format of .m4a files. The
 *   module reads the first audio track. It takes the decoder configuration
 *   from the esds box of the mp4a sample entry. It takes the position and
 *   size of every sample (one AAC frame) from the sample table boxes stsz,
 *   stsc and stco or co64. The sample data may lie anywhere in the file. */
#include "aac.h"
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

/* ---- ADTS ---- */

struct adts_header {
    unsigned object_type, rate_index, channel_config;
    size_t header_len, frame_len;
    unsigned blocks;
    int crc;
};

/* Returns the length of an ID3v2 tag at the start of data, or 0. */
static size_t id3_length(const uint8_t *data, size_t len)
{
    if (len < 10 || memcmp(data, "ID3", 3) != 0 || ((data[6] | data[7] | data[8] | data[9]) & 0x80))
        return 0;
    size_t size = (size_t)data[6] << 21 | (size_t)data[7] << 14 | (size_t)data[8] << 7 | data[9];
    return 10 + size + ((data[5] & 0x10) ? 10 : 0);
}

static int parse_adts(const uint8_t *p, size_t len, struct adts_header *h)
{
    if (len < 7 || p[0] != 0xff || (p[1] & 0xf6) != 0xf0)
        return -EINVAL;                 /* syncword, layer 0 */
    h->crc = !(p[1] & 1);
    h->object_type = (p[2] >> 6) + 1;
    h->rate_index = (p[2] >> 2) & 15;
    h->channel_config = (p[2] & 1) << 2 | p[3] >> 6;
    h->frame_len = (size_t)(p[3] & 3) << 11 | (size_t)p[4] << 3 | p[5] >> 5;
    h->blocks = (p[6] & 3) + 1;
    h->header_len = h->crc ? 9 : 7;
    if (h->rate_index > 12 || h->frame_len < h->header_len)
        return -EINVAL;
    return 0;
}

int aac_adts_probe(const uint8_t *data, size_t len)
{
    size_t at = id3_length(data, len);
    struct adts_header h, h2;
    if (at >= len || parse_adts(data + at, len - at, &h) < 0)
        return 0;
    if (at + h.frame_len + 7 <= len)
        return parse_adts(data + at + h.frame_len, len - at - h.frame_len, &h2) == 0 &&
                       h2.rate_index == h.rate_index && h2.channel_config == h.channel_config
                   ? 90
                   : 0;
    return 60;
}

static int adts_open(struct aac_stream *s)
{
    struct adts_header h;
    s->next = id3_length(s->data, s->len);
    if (s->next >= s->len || parse_adts(s->data + s->next, s->len - s->next, &h) < 0)
        return -EINVAL;
    if (h.blocks > 1 && h.crc)
        return -ENOTSUP;                /* several blocks, each with its own CRC */
    if (h.channel_config == 0)
        return -ENOTSUP;                /* the channels are given by a PCE in each frame */
    memset(&s->cfg, 0, sizeof s->cfg);
    s->cfg.object_type = h.object_type;
    s->cfg.rate_index = h.rate_index;
    s->cfg.channel_config = h.channel_config;
    s->adts = 1;
    return aac_config_finish(&s->cfg);
}

/* ---- MP4 ---- */

/* Finds the first box of type type among the boxes in data. */
static const uint8_t *find_box(const uint8_t *data, size_t len, uint32_t type, size_t *body_len)
{
    size_t at = 0;
    while (at + 8 <= len) {
        uint64_t size = be32(data + at);
        size_t header = 8;
        if (size == 1) {
            if (at + 16 > len)
                return NULL;
            size = be64(data + at + 8);
            header = 16;
        } else if (size == 0) {
            size = len - at;
        }
        if (size < header || size > len - at)
            return NULL;
        if (be32(data + at + 4) == type) {
            *body_len = (size_t)size - header;
            return data + at + header;
        }
        at += (size_t)size;
    }
    return NULL;
}

/* Follows a path of nested boxes. */
static const uint8_t *find_path(const uint8_t *data, size_t len, const uint32_t *path, unsigned depth,
                                size_t *body_len)
{
    for (unsigned i = 0; i < depth && data; i++)
        data = find_box(data, len, path[i], &len);
    *body_len = len;
    return data;
}

/* Reads the length field of an MPEG-4 descriptor. The field has up to four
 * bytes, each with seven bits of length and a continuation bit. */
static int descriptor(const uint8_t **p, const uint8_t *end, unsigned *tag, size_t *len)
{
    if (*p >= end)
        return -EINVAL;
    *tag = *(*p)++;
    *len = 0;
    for (int i = 0; i < 4; i++) {
        if (*p >= end)
            return -EINVAL;
        uint8_t b = *(*p)++;
        *len = *len << 7 | (b & 0x7f);
        if (!(b & 0x80))
            break;
    }
    return *len <= (size_t)(end - *p) ? 0 : -EINVAL;
}

/* Reads the decoder configuration in an esds box. This is the object type
 * indication of the DecoderConfigDescriptor and, if present, the
 * AudioSpecificConfig. */
static int parse_esds(const uint8_t *p, size_t len, unsigned *oti, const uint8_t **asc, size_t *asc_len)
{
    const uint8_t *end = p + len;
    unsigned tag;
    size_t n;
    *asc = NULL;
    *asc_len = 0;
    p += 4;                             /* version and flags */
    if (p > end || descriptor(&p, end, &tag, &n) < 0 || tag != 3 || n < 3)
        return -EINVAL;
    end = p + n;
    unsigned flags = p[2];
    p += 3;                             /* ES_ID, flags */
    if (flags & 0x80)
        p += 2;                         /* dependsOn_ES_ID */
    if ((flags & 0x40) && p < end)
        p += 1 + *p;                    /* URL */
    if (flags & 0x20)
        p += 2;                         /* OCR_ES_Id */
    if (p > end || descriptor(&p, end, &tag, &n) < 0 || tag != 4 || n < 13)
        return -EINVAL;
    end = p + n;
    *oti = p[0];
    p += 13;
    if (p < end && descriptor(&p, end, &tag, &n) == 0 && tag == 5) {
        *asc = p;
        *asc_len = n;
    }
    return 0;
}

/* Reads the mp4a sample entry and the decoder configuration in its esds
 * box. QuickTime files place the esds box inside a wave box. */
static int parse_mp4a(struct aac_stream *s, const uint8_t *p, size_t len)
{
    if (len < 28)
        return -EINVAL;
    unsigned version = be16(p + 8), channels = be16(p + 16);
    size_t skip = 28 + (version == 1 ? 16 : version == 2 ? 36 : 0);
    if (skip > len)
        return -EINVAL;
    size_t blen;
    const uint8_t *esds = find_box(p + skip, len - skip, FOURCC('e', 's', 'd', 's'), &blen);
    if (!esds) {
        const uint8_t *wave = find_box(p + skip, len - skip, FOURCC('w', 'a', 'v', 'e'), &blen);
        esds = wave ? find_box(wave, blen, FOURCC('e', 's', 'd', 's'), &blen) : NULL;
    }
    unsigned oti;
    const uint8_t *asc;
    size_t asc_len;
    if (!esds || parse_esds(esds, blen, &oti, &asc, &asc_len) < 0)
        return -EINVAL;
    if (oti != 0x40 && oti != 0x67)
        return -ENOTSUP;                /* not MPEG-4 audio or MPEG-2 AAC-LC */
    if (asc)
        return aac_parse_config(asc, asc_len, &s->cfg);
    if (oti != 0x67)
        return -EINVAL;
    /* MPEG-2 AAC-LC without a configuration: the sample entry provides the
     * rate and the channel count. */
    memset(&s->cfg, 0, sizeof s->cfg);
    s->cfg.object_type = 2;
    unsigned rate = be32(p + 24) >> 16;
    static const unsigned rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000, 7350 };
    s->cfg.rate_index = 13;
    for (unsigned i = 0; i < 13; i++)
        if (rates[i] == rate)
            s->cfg.rate_index = i;
    s->cfg.channel_config = channels;
    return aac_config_finish(&s->cfg);
}

/* Computes the offset and size of every sample from the sample table. */
static int parse_sample_table(struct aac_stream *s, const uint8_t *stbl, size_t len)
{
    size_t stsz_len, stsc_len, stco_len;
    const uint8_t *stsz = find_box(stbl, len, FOURCC('s', 't', 's', 'z'), &stsz_len);
    const uint8_t *stsc = find_box(stbl, len, FOURCC('s', 't', 's', 'c'), &stsc_len);
    const uint8_t *stco = find_box(stbl, len, FOURCC('s', 't', 'c', 'o'), &stco_len);
    int wide = 0;
    if (!stco) {
        stco = find_box(stbl, len, FOURCC('c', 'o', '6', '4'), &stco_len);
        wide = 1;
    }
    if (!stsz || !stsc || !stco || stsz_len < 12 || stsc_len < 8 || stco_len < 8)
        return -EINVAL;
    uint32_t fixed = be32(stsz + 4), count = be32(stsz + 8);
    uint32_t entries = be32(stsc + 4), chunks = be32(stco + 4);
    if ((!fixed && (uint64_t)count * 4 > stsz_len - 12) || (uint64_t)entries * 12 > stsc_len - 8 ||
        (uint64_t)chunks * (wide ? 8 : 4) > stco_len - 8 || count > (1u << 24))
        return -EINVAL;
    s->offsets = malloc(sizeof *s->offsets * (count ? count : 1));
    s->sizes = malloc(sizeof *s->sizes * (count ? count : 1));
    if (!s->offsets || !s->sizes)
        return -ENOMEM;
    uint32_t sample = 0;
    for (uint32_t e = 0; e < entries && sample < count; e++) {
        const uint8_t *entry = stsc + 8 + 12 * e;
        uint32_t first = be32(entry), per_chunk = be32(entry + 4);
        uint32_t last = e + 1 < entries ? be32(entry + 12) : chunks + 1;
        if (first == 0 || last < first || last > chunks + 1)
            return -EINVAL;
        for (uint32_t c = first; c < last && sample < count; c++) {
            uint64_t at = wide ? be64(stco + 8 + 8 * (c - 1)) : be32(stco + 8 + 4 * (c - 1));
            for (uint32_t i = 0; i < per_chunk && sample < count; i++, sample++) {
                uint32_t size = fixed ? fixed : be32(stsz + 12 + 4 * sample);
                if (at > s->len || size > s->len - at)
                    return -EINVAL;
                s->offsets[sample] = at;
                s->sizes[sample] = size;
                at += size;
            }
        }
    }
    if (sample != count)
        return -EINVAL;
    s->count = count;
    return 0;
}

int aac_mp4_probe(const uint8_t *data, size_t len)
{
    if (len < 12 || memcmp(data + 4, "ftyp", 4) != 0)
        return 0;
    if (!memcmp(data + 8, "M4A ", 4) || !memcmp(data + 8, "M4B ", 4) || !memcmp(data + 8, "M4P ", 4))
        return 90;
    /* A general MP4 file may contain an AAC track. */
    return 40;
}

static int mp4_open(struct aac_stream *s)
{
    size_t moov_len;
    const uint8_t *moov = find_box(s->data, s->len, FOURCC('m', 'o', 'o', 'v'), &moov_len);
    if (!moov)
        return -EINVAL;
    int result = -EINVAL;
    size_t at = 0;
    /* Visit each trak box in turn until one is an audio track with mp4a. */
    while (at + 8 <= moov_len) {
        size_t size = be32(moov + at), trak_len;
        if (size < 8 || size > moov_len - at)
            break;
        if (be32(moov + at + 4) != FOURCC('t', 'r', 'a', 'k')) {
            at += size;
            continue;
        }
        const uint8_t *trak = moov + at + 8;
        trak_len = size - 8;
        at += size;
        static const uint32_t hdlr_path[] = { FOURCC('m', 'd', 'i', 'a'), FOURCC('h', 'd', 'l', 'r') };
        static const uint32_t stbl_path[] = { FOURCC('m', 'd', 'i', 'a'), FOURCC('m', 'i', 'n', 'f'),
                                              FOURCC('s', 't', 'b', 'l') };
        size_t hdlr_len, stbl_len, stsd_len;
        const uint8_t *hdlr = find_path(trak, trak_len, hdlr_path, 2, &hdlr_len);
        if (!hdlr || hdlr_len < 12 || be32(hdlr + 8) != FOURCC('s', 'o', 'u', 'n'))
            continue;
        const uint8_t *stbl = find_path(trak, trak_len, stbl_path, 3, &stbl_len);
        const uint8_t *stsd = stbl ? find_box(stbl, stbl_len, FOURCC('s', 't', 's', 'd'), &stsd_len) : NULL;
        if (!stsd || stsd_len < 8)
            continue;
        size_t entry_len;
        const uint8_t *mp4a = find_box(stsd + 8, stsd_len - 8, FOURCC('m', 'p', '4', 'a'), &entry_len);
        if (!mp4a)
            continue;
        result = parse_mp4a(s, mp4a, entry_len);
        if (result == 0)
            result = parse_sample_table(s, stbl, stbl_len);
        return result;
    }
    return result;
}

/* ---- common ---- */

int aac_stream_open(struct aac_stream *s, const uint8_t *data, size_t len)
{
    memset(s, 0, sizeof *s);
    s->data = data;
    s->len = len;
    int err;
    if (aac_mp4_probe(data, len))
        err = mp4_open(s);
    else
        err = adts_open(s);
    if (err)
        aac_stream_close(s);
    return err;
}

void aac_stream_close(struct aac_stream *s)
{
    free(s->offsets);
    free(s->sizes);
    s->offsets = NULL;
    s->sizes = NULL;
}

int aac_stream_next(struct aac_stream *s, const uint8_t **frame, size_t *len, unsigned *blocks)
{
    if (!s->adts) {
        if (s->index >= s->count)
            return 0;
        *frame = s->data + s->offsets[s->index];
        *len = s->sizes[s->index++];
        *blocks = 1;
        return 1;
    }
    size_t left = s->len - s->next;
    if (left == 0 || (left == 128 && !memcmp(s->data + s->next, "TAG", 3)))
        return 0;                       /* end of the stream, or an ID3v1 tag */
    struct adts_header h;
    if (parse_adts(s->data + s->next, left, &h) < 0 || h.frame_len > left || h.rate_index != s->cfg.rate_index ||
        h.channel_config != s->cfg.channel_config || (h.blocks > 1 && h.crc))
        return -EBADMSG;
    *frame = s->data + s->next + h.header_len;
    *len = h.frame_len - h.header_len;
    *blocks = h.blocks;
    s->next += h.frame_len;
    return 1;
}

long aac_stream_frames(const struct aac_stream *s)
{
    if (!s->adts)
        return (long)s->count * AAC_FRAME;
    long total = 0;
    size_t at = id3_length(s->data, s->len);
    struct adts_header h;
    while (at < s->len && parse_adts(s->data + at, s->len - at, &h) == 0 && h.frame_len <= s->len - at) {
        total += (long)h.blocks * AAC_FRAME;
        at += h.frame_len;
    }
    return total;
}
