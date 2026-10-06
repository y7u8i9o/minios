/* Stream configuration. This file handles the AudioSpecificConfig of MP4
 * files (section 1.6.2.1 of ISO/IEC 14496-3), the program configuration
 * element (section 4.4.1.1) and channel configurations 1 to 7. A channel
 * configuration fixes the channel elements of each frame and their speaker
 * positions. */
#include "aac.h"
#include <string.h>

static const unsigned rates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

/* Returns the rate index whose tables apply to an explicitly coded sample
 * rate. The standard assigns each rate to the nearest of the defined
 * rates. */
static unsigned rate_index_of(unsigned rate)
{
    static const unsigned bounds[] = { 92017, 75132, 55426, 46009, 37566, 27713, 23004, 18783, 13856, 11502, 9391 };
    for (unsigned i = 0; i < sizeof bounds / sizeof bounds[0]; i++)
        if (rate >= bounds[i])
            return i;
    return 11;
}

/* The channel elements of channel configurations 1 to 7 and, for each WAV
 * channel position, the decoder channel that feeds it. The decoder numbers
 * its channels in element order, so the centre channel comes first.
 * Configuration 6, for example, is centre, left, right, left surround,
 * right surround and LFE. */
static const struct {
    unsigned nelements;
    uint8_t elements[5];
    unsigned channels;
    uint8_t order[8];
} layouts[8] = {
    [1] = { 1, { AAC_SCE }, 1, { 0 } },
    [2] = { 1, { AAC_CPE }, 2, { 0, 1 } },
    [3] = { 2, { AAC_SCE, AAC_CPE }, 3, { 1, 2, 0 } },
    [4] = { 3, { AAC_SCE, AAC_CPE, AAC_SCE }, 4, { 1, 2, 0, 3 } },
    [5] = { 3, { AAC_SCE, AAC_CPE, AAC_CPE }, 5, { 1, 2, 0, 3, 4 } },
    [6] = { 4, { AAC_SCE, AAC_CPE, AAC_CPE, AAC_LFE }, 6, { 1, 2, 0, 5, 3, 4 } },
    [7] = { 5, { AAC_SCE, AAC_CPE, AAC_CPE, AAC_CPE, AAC_LFE }, 8, { 3, 4, 0, 7, 5, 6, 1, 2 } },
};

int aac_config_finish(struct aac_config *cfg)
{
    if (cfg->object_type != 2)
        return -ENOTSUP;
    if (cfg->rate_index > 12)
        return -EINVAL;
    if (!cfg->rate)
        cfg->rate = rates[cfg->rate_index];
    if (cfg->channel_config == 0)
        return cfg->nelements ? 0 : -EINVAL;
    if (cfg->channel_config > 7)
        return -ENOTSUP;
    cfg->nelements = layouts[cfg->channel_config].nelements;
    memcpy(cfg->elements, layouts[cfg->channel_config].elements, cfg->nelements);
    cfg->channels = layouts[cfg->channel_config].channels;
    memcpy(cfg->order, layouts[cfg->channel_config].order, cfg->channels);
    return 0;
}

/* Appends the elements of one group of a program configuration element.
 * The group has count entries. Each entry consists of an optional is_cpe
 * bit and a 4-bit tag. */
static int add_group(struct aac_bits *b, struct aac_config *cfg, unsigned count, int may_be_pair, uint8_t single)
{
    for (unsigned i = 0; i < count; i++) {
        int pair = may_be_pair ? (int)aac_bit(b) : 0;
        aac_skip(b, 4);                 /* element_instance_tag */
        unsigned n = pair ? 2 : 1;
        if (cfg->nelements == AAC_MAX_ELEMENTS || cfg->channels + n > AAC_MAX_CHANNELS)
            return -ENOTSUP;
        cfg->elements[cfg->nelements++] = pair ? AAC_CPE : single;
        cfg->channels += n;
    }
    return 0;
}

int aac_parse_pce(struct aac_bits *b, size_t start, struct aac_config *cfg)
{
    aac_skip(b, 4);                     /* element_instance_tag */
    unsigned object_type = aac_get(b, 2) + 1;
    unsigned rate_index = aac_get(b, 4);
    unsigned front = aac_get(b, 4), side = aac_get(b, 4), back = aac_get(b, 4);
    unsigned lfe = aac_get(b, 2), assoc = aac_get(b, 3), cc = aac_get(b, 4);
    if (aac_bit(b))
        aac_skip(b, 4);                 /* mono_mixdown_element_number */
    if (aac_bit(b))
        aac_skip(b, 4);                 /* stereo_mixdown_element_number */
    if (aac_bit(b))
        aac_skip(b, 3);                 /* matrix_mixdown_idx, pseudo_surround_enable */
    struct aac_config pce = *cfg;
    pce.nelements = 0;
    pce.channels = 0;
    int err = add_group(b, &pce, front, 1, AAC_SCE);
    if (!err)
        err = add_group(b, &pce, side, 1, AAC_SCE);
    if (!err)
        err = add_group(b, &pce, back, 1, AAC_SCE);
    if (!err)
        err = add_group(b, &pce, lfe, 0, AAC_LFE);
    if (err)
        return err;
    aac_skip(b, 4 * assoc);             /* assoc_data_element_tag_select */
    aac_skip(b, 5 * cc);                /* cc_element_is_ind_sw, valid_cc_element_tag_select */
    aac_align(b, start);
    aac_skip(b, 8 * aac_get(b, 8));     /* comment field */
    if (b->overrun || pce.channels == 0)
        return -EINVAL;
    if (object_type != 2)
        return -ENOTSUP;
    pce.rate_index = rate_index;
    /* A program without a standard layout retains the order of its elements:
     * front, side and back channels, then LFE. */
    for (unsigned c = 0; c < pce.channels; c++)
        pce.order[c] = (uint8_t)c;
    *cfg = pce;
    return 0;
}

static unsigned object_type(struct aac_bits *b)
{
    unsigned type = aac_get(b, 5);
    return type == 31 ? 32 + aac_get(b, 6) : type;
}

static void sampling_frequency(struct aac_bits *b, unsigned *index, unsigned *rate)
{
    *index = aac_get(b, 4);
    *rate = 0;
    if (*index == 15) {
        *rate = aac_get(b, 24);
        *index = rate_index_of(*rate);
    }
}

int aac_parse_config(const uint8_t *data, size_t len, struct aac_config *cfg)
{
    struct aac_bits b;
    aac_bits_init(&b, data, len);
    memset(cfg, 0, sizeof *cfg);
    cfg->object_type = object_type(&b);
    sampling_frequency(&b, &cfg->rate_index, &cfg->rate);
    cfg->channel_config = aac_get(&b, 4);
    if (cfg->object_type == 5 || cfg->object_type == 29) {
        /* HE-AAC with explicit signalling: the extension rate is the output
         * rate of SBR, and the core object type follows. The decoder
         * decodes the core at the first rate. */
        unsigned ext_index, ext_rate;
        sampling_frequency(&b, &ext_index, &ext_rate);
        cfg->sbr = 1;
        cfg->object_type = object_type(&b);
    }
    if (cfg->object_type != 2)
        return b.overrun ? -EINVAL : -ENOTSUP;
    /* GASpecificConfig (section 4.4.1). */
    if (aac_bit(&b))
        return -ENOTSUP;                /* frame length 960 */
    if (aac_bit(&b))
        aac_skip(&b, 14);               /* coreCoderDelay */
    aac_skip(&b, 1);                    /* extensionFlag, always 0 for AAC-LC */
    if (cfg->channel_config == 0) {
        int err = aac_parse_pce(&b, 0, cfg);
        if (err)
            return err;
    }
    if (b.overrun)
        return -EINVAL;
    return aac_config_finish(cfg);
}
