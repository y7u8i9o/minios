/* Decoding of AAC-LC raw data blocks (sections 4.4 to 4.6 of ISO/IEC
 * 14496-3).
 *
 * A raw_data_block is a sequence of syntax elements that ends with an END
 * element. A single channel element (SCE) or an LFE element carries one
 * individual channel stream, and a channel pair element (CPE) carries two.
 * For every channel stream the decoder reads the window sequence and the
 * grouping, the section data that assigns a codebook to each scale factor
 * band, the scale factors, the optional pulse and TNS data, and the
 * Huffman-coded spectral data. It then reconstructs the spectrum in these
 * steps:
 *
 * 1. Pulses are added to the quantised values.
 * 2. The values are inverse quantised and scaled by the scale factors.
 * 3. Perceptual noise substitution fills the noise bands.
 * 4. For channel pairs, M/S stereo and then intensity stereo are applied.
 * 5. TNS filters each window.
 * 6. The filter bank converts the spectrum into samples.
 *
 * Coupling channel elements, gain control and the prediction tools of the
 * other AAC profiles are not part of AAC-LC, so the decoder rejects them.
 * It skips data stream elements and fill elements, including SBR extension
 * data. */
#include "aac.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ZERO_HCB      0
#define FIRST_PAIR_HCB 5
#define ESC_HCB       11
#define NOISE_HCB     13
#define INTENSITY_HCB2 14
#define INTENSITY_HCB 15

/* One TNS filter of a window. */
struct tns_filter {
    unsigned length, order, direction, coef_res;
    int coef[20];
};

/* All the information that the bitstream gives for one channel stream of one
 * frame. */
struct ics {
    unsigned sequence, shape;
    unsigned max_sfb;
    unsigned num_windows, num_groups;
    unsigned group_len[8];
    const uint16_t *offsets;            /* band offsets for one window */
    unsigned num_swb, tns_max;
    uint8_t cb[8][AAC_MAX_SFB];         /* codebook per group and band */
    int sf[8][AAC_MAX_SFB];             /* scale factor, intensity position or noise energy */
    int pulse_present;
    unsigned pulse_count, pulse_start;
    uint8_t pulse_offset[4], pulse_amp[4];
    int tns_present;
    unsigned tns_filters[8];
    struct tns_filter tns[8][4];
    int quant[AAC_FRAME];               /* quantised values; short windows use 8 blocks of 128 */
    float spec[AAC_FRAME];
};

struct aac_decoder {
    struct aac_config cfg;
    const struct aac_bands *bands;
    struct aac_huffman sf_huff, spec_huff[11];
    struct aac_filterbank fb;
    struct aac_channel ch[AAC_MAX_CHANNELS];
    struct ics ics[2];                  /* streams of the current element */
    uint8_t ms_used[8][AAC_MAX_SFB];
    float pcm[AAC_MAX_CHANNELS][AAC_FRAME];
    float pow43[8192];                  /* |q|^(4/3) */
    uint32_t noise_seed;
};

struct aac_decoder *aac_decoder_new(const struct aac_config *cfg)
{
    struct aac_decoder *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->cfg = *cfg;
    d->bands = &aac_bands[cfg->rate_index];
    int err = aac_huffman_build(&d->sf_huff, &aac_sf_codebook);
    for (unsigned i = 0; i < 11 && !err; i++)
        err = aac_huffman_build(&d->spec_huff[i], &aac_spectral_codebooks[i]);
    if (!err)
        err = aac_filterbank_init(&d->fb);
    if (err) {
        aac_decoder_free(d);
        return NULL;
    }
    for (unsigned i = 0; i < 8192; i++)
        d->pow43[i] = (float)pow(i, 4.0 / 3.0);
    d->noise_seed = 0x1f2e3d4c;
    return d;
}

void aac_decoder_free(struct aac_decoder *d)
{
    if (!d)
        return;
    aac_huffman_free(&d->sf_huff);
    for (unsigned i = 0; i < 11; i++)
        aac_huffman_free(&d->spec_huff[i]);
    aac_filterbank_free(&d->fb);
    free(d);
}

/* ---- the bitstream ---- */

/* ics_info (section 4.4.2.1). */
static int read_ics_info(struct aac_decoder *d, struct aac_bits *b, struct ics *s)
{
    if (aac_bit(b))
        return -EBADMSG;                /* ics_reserved_bit */
    s->sequence = aac_get(b, 2);
    s->shape = aac_bit(b);
    if (s->sequence == AAC_EIGHT_SHORT) {
        s->max_sfb = aac_get(b, 4);
        unsigned grouping = aac_get(b, 7);
        s->num_windows = 8;
        s->num_groups = 1;
        s->group_len[0] = 1;
        for (unsigned w = 1; w < 8; w++) {
            if (grouping & (1u << (7 - w)))
                s->group_len[s->num_groups - 1]++;
            else
                s->group_len[s->num_groups++] = 1;
        }
        s->offsets = d->bands->short_offsets;
        s->num_swb = d->bands->short_count;
        s->tns_max = d->bands->tns_short;
    } else {
        s->max_sfb = aac_get(b, 6);
        s->num_windows = 1;
        s->num_groups = 1;
        s->group_len[0] = 1;
        s->offsets = d->bands->long_offsets;
        s->num_swb = d->bands->long_count;
        s->tns_max = d->bands->tns_long;
        if (aac_bit(b))
            return -ENOTSUP;            /* predictor data: AAC Main or LTP */
    }
    return s->max_sfb > s->num_swb ? -EBADMSG : 0;
}

/* section_data (section 4.4.2.7). */
static int read_section_data(struct aac_bits *b, struct ics *s)
{
    unsigned bits = s->num_windows == 8 ? 3 : 5, esc = (1u << bits) - 1;
    for (unsigned g = 0; g < s->num_groups; g++) {
        unsigned k = 0;
        while (k < s->max_sfb) {
            unsigned cb = aac_get(b, 4);
            if (cb == 12)
                return -EBADMSG;        /* reserved codebook */
            unsigned len = 0, incr;
            do {
                incr = aac_get(b, bits);
                len += incr;
            } while (incr == esc && !b->overrun);
            if (b->overrun || k + len > s->max_sfb)
                return -EBADMSG;
            for (unsigned i = 0; i < len; i++)
                s->cb[g][k++] = (uint8_t)cb;
        }
        for (; k < s->num_swb; k++)
            s->cb[g][k] = ZERO_HCB;
    }
    return 0;
}

/* scale_factor_data (section 4.4.2.7). The bitstream codes scale factors,
 * intensity positions and noise energies each as a difference from the
 * previous value of the same kind. The first noise energy is a 9-bit value
 * relative to the global gain minus 90. */
static int read_scale_factors(struct aac_decoder *d, struct aac_bits *b, struct ics *s, unsigned global_gain)
{
    int gain = (int)global_gain, position = 0, noise = (int)global_gain - 90;
    int first_noise = 1;
    for (unsigned g = 0; g < s->num_groups; g++)
        for (unsigned k = 0; k < s->max_sfb; k++) {
            unsigned cb = s->cb[g][k];
            if (cb == ZERO_HCB) {
                s->sf[g][k] = 0;
                continue;
            }
            int delta;
            if (cb == NOISE_HCB && first_noise) {
                delta = (int)aac_get(b, 9) - 256;
                first_noise = 0;
            } else {
                int sym = aac_huffman_decode(&d->sf_huff, b);
                if (sym < 0)
                    return -EBADMSG;
                delta = sym - 60;
            }
            if (cb == INTENSITY_HCB || cb == INTENSITY_HCB2)
                s->sf[g][k] = position += delta;
            else if (cb == NOISE_HCB)
                s->sf[g][k] = noise += delta;
            else {
                gain += delta;
                if (gain < 0 || gain > 255)
                    return -EBADMSG;
                s->sf[g][k] = gain;
            }
        }
    return 0;
}

/* pulse_data (section 4.4.2.7). Only long windows may carry it. */
static int read_pulse_data(struct aac_bits *b, struct ics *s)
{
    s->pulse_count = aac_get(b, 2) + 1;
    s->pulse_start = aac_get(b, 6);
    for (unsigned i = 0; i < s->pulse_count; i++) {
        s->pulse_offset[i] = (uint8_t)aac_get(b, 5);
        s->pulse_amp[i] = (uint8_t)aac_get(b, 4);
    }
    return s->num_windows == 8 || s->pulse_start >= s->num_swb ? -EBADMSG : 0;
}

/* tns_data (section 4.4.2.7). */
static int read_tns_data(struct aac_bits *b, struct ics *s)
{
    int is_short = s->num_windows == 8;
    for (unsigned w = 0; w < s->num_windows; w++) {
        s->tns_filters[w] = aac_get(b, is_short ? 1 : 2);
        if (!s->tns_filters[w])
            continue;
        unsigned coef_res = aac_bit(b) + 3;
        for (unsigned f = 0; f < s->tns_filters[w]; f++) {
            struct tns_filter *t = &s->tns[w][f];
            t->length = aac_get(b, is_short ? 4 : 6);
            t->order = aac_get(b, is_short ? 3 : 5);
            t->coef_res = coef_res;
            if (t->order > (is_short ? 7u : 12u))
                return -EBADMSG;        /* above TNS_MAX_ORDER of AAC-LC */
            if (!t->order)
                continue;
            t->direction = aac_bit(b);
            unsigned compress = aac_bit(b), bits = coef_res - compress;
            for (unsigned i = 0; i < t->order; i++) {
                int v = (int)aac_get(b, bits);
                if (v & (1 << (bits - 1)))
                    v -= 1 << bits;     /* two's complement */
                t->coef[i] = v;
            }
        }
    }
    return 0;
}

/* The escape sequence of codebook 11 (section 4.6.3.3) consists of N one
 * bits, a zero bit and an (N + 4)-bit value. The result is 2^(N + 4) plus
 * the value. */
static int read_escape(struct aac_bits *b)
{
    unsigned n = 0;
    while (aac_bit(b)) {
        if (++n > 8 || b->overrun)
            return -1;
    }
    return (1 << (n + 4)) + (int)aac_get(b, n + 4);
}

/* spectral_data (section 4.4.2.7). Each codeword contains four values for
 * codebooks 1 to 4 and two values for the other codebooks. Codebooks 1, 2,
 * 5 and 6 code signed values. The other codebooks code magnitudes, and a
 * sign bit follows each value that is not zero. */
static int read_spectral_data(struct aac_decoder *d, struct aac_bits *b, struct ics *s)
{
    static const struct {
        unsigned dim, base;
        int is_signed;
    } books[12] = {
        [1] = { 4, 3, 1 },  [2] = { 4, 3, 1 },  [3] = { 4, 3, 0 },  [4] = { 4, 3, 0 },
        [5] = { 2, 9, 1 },  [6] = { 2, 9, 1 },  [7] = { 2, 8, 0 },  [8] = { 2, 8, 0 },
        [9] = { 2, 13, 0 }, [10] = { 2, 13, 0 }, [11] = { 2, 17, 0 },
    };
    memset(s->quant, 0, sizeof s->quant);
    unsigned window = 0;
    for (unsigned g = 0; g < s->num_groups; g++) {
        for (unsigned k = 0; k < s->max_sfb; k++) {
            unsigned cb = s->cb[g][k];
            if (cb == ZERO_HCB || cb >= NOISE_HCB)
                continue;
            unsigned dim = books[cb].dim, base = books[cb].base;
            int offset = books[cb].is_signed ? (int)(base / 2) : 0;
            for (unsigned w = 0; w < s->group_len[g]; w++) {
                int *q = s->quant + (window + w) * AAC_SHORT;
                for (unsigned i = s->offsets[k]; i < s->offsets[k + 1]; i += dim) {
                    int sym = aac_huffman_decode(&d->spec_huff[cb - 1], b);
                    if (sym < 0)
                        return -EBADMSG;
                    int v[4];
                    for (unsigned j = dim; j-- > 0;) {
                        v[j] = sym % (int)base - offset;
                        sym /= (int)base;
                    }
                    if (!books[cb].is_signed)
                        for (unsigned j = 0; j < dim; j++)
                            if (v[j] && aac_bit(b))
                                v[j] = -v[j];
                    if (cb == ESC_HCB)
                        for (unsigned j = 0; j < 2; j++)
                            if (v[j] == 16 || v[j] == -16) {
                                int e = read_escape(b);
                                if (e < 0)
                                    return -EBADMSG;
                                v[j] = v[j] < 0 ? -e : e;
                            }
                    for (unsigned j = 0; j < dim; j++)
                        q[i + j] = v[j];
                }
            }
        }
        window += s->group_len[g];
    }
    return b->overrun ? -EBADMSG : 0;
}

/* individual_channel_stream (section 4.4.2.7). For a channel pair with a
 * common window, the caller has already read ics_info into s. */
static int read_ics(struct aac_decoder *d, struct aac_bits *b, struct ics *s, int common_window)
{
    unsigned global_gain = aac_get(b, 8);
    int err;
    if (!common_window && (err = read_ics_info(d, b, s)) < 0)
        return err;
    if ((err = read_section_data(b, s)) < 0 || (err = read_scale_factors(d, b, s, global_gain)) < 0)
        return err;
    s->pulse_present = aac_bit(b);
    if (s->pulse_present && (err = read_pulse_data(b, s)) < 0)
        return err;
    s->tns_present = aac_bit(b);
    if (s->tns_present && (err = read_tns_data(b, s)) < 0)
        return err;
    if (aac_bit(b))
        return -ENOTSUP;                /* gain control: AAC SSR */
    return read_spectral_data(d, b, s);
}

/* ---- spectral reconstruction ---- */

/* 2^(e / 4) for any integer e. */
static float pow2_quarter(int e)
{
    static const float frac[4] = { 1.0f, 1.18920712f, 1.41421356f, 1.68179283f };
    return ldexpf(frac[e & 3], e >> 2);
}

static uint32_t next_random(struct aac_decoder *d)
{
    d->noise_seed = d->noise_seed * 1664525u + 1013904223u;
    return d->noise_seed;
}

/* Applies pulses, inverse quantisation and scaling, and noise substitution.
 * The standard requires correlated noise in channel pairs. Therefore, when
 * noise_from is given, a noise band that is also a noise band in that
 * stream and uses M/S takes the noise of that stream. */
static void reconstruct(struct aac_decoder *d, struct ics *s, const struct ics *noise_from)
{
    if (s->pulse_present) {
        unsigned k = s->offsets[s->pulse_start];
        for (unsigned i = 0; i < s->pulse_count; i++) {
            k += s->pulse_offset[i];
            if (k >= AAC_FRAME)
                break;
            s->quant[k] += s->quant[k] >= 0 ? s->pulse_amp[i] : -(int)s->pulse_amp[i];
        }
    }
    memset(s->spec, 0, sizeof s->spec);
    unsigned window = 0;
    for (unsigned g = 0; g < s->num_groups; g++) {
        for (unsigned k = 0; k < s->max_sfb; k++) {
            unsigned cb = s->cb[g][k];
            unsigned lo = s->offsets[k], hi = s->offsets[k + 1];
            for (unsigned w = 0; w < s->group_len[g]; w++) {
                unsigned base = (window + w) * AAC_SHORT;
                const int *q = s->quant + base;
                float *x = s->spec + base;
                if (cb == NOISE_HCB) {
                    if (noise_from && noise_from->cb[g][k] == NOISE_HCB && d->ms_used[g][k]) {
                        /* Same noise as the other channel, at the energy of
                         * this channel. */
                        float energy = 0;
                        for (unsigned i = lo; i < hi; i++)
                            energy += noise_from->spec[base + i] * noise_from->spec[base + i];
                        float scale = energy > 0 ? pow2_quarter(s->sf[g][k]) / sqrtf(energy) : 0;
                        for (unsigned i = lo; i < hi; i++)
                            x[i] = noise_from->spec[base + i] * scale;
                        continue;
                    }
                    float energy = 0;
                    for (unsigned i = lo; i < hi; i++) {
                        x[i] = (float)(int32_t)next_random(d);
                        energy += x[i] * x[i];
                    }
                    float scale = pow2_quarter(s->sf[g][k]) / sqrtf(energy);
                    for (unsigned i = lo; i < hi; i++)
                        x[i] *= scale;
                } else if (cb != ZERO_HCB && cb < NOISE_HCB) {
                    float scale = pow2_quarter(s->sf[g][k] - 100);
                    for (unsigned i = lo; i < hi; i++) {
                        int v = q[i], a = v < 0 ? -v : v;
                        float m = a < 8192 ? d->pow43[a] : (float)pow(a, 4.0 / 3.0);
                        x[i] = (v < 0 ? -m : m) * scale;
                    }
                }
            }
        }
        window += s->group_len[g];
    }
}

/* M/S stereo (section 4.6.8.1): left = mid + side and right = mid - side.
 * The conversion applies to bands that neither channel codes as noise or
 * intensity. */
static void apply_ms(struct aac_decoder *d, struct ics *l, struct ics *r)
{
    unsigned window = 0;
    for (unsigned g = 0; g < l->num_groups; g++) {
        for (unsigned k = 0; k < l->max_sfb; k++) {
            if (!d->ms_used[g][k] || l->cb[g][k] >= NOISE_HCB || r->cb[g][k] >= NOISE_HCB)
                continue;
            for (unsigned w = 0; w < l->group_len[g]; w++) {
                unsigned base = (window + w) * AAC_SHORT;
                for (unsigned i = l->offsets[k]; i < l->offsets[k + 1]; i++) {
                    float m = l->spec[base + i], sd = r->spec[base + i];
                    l->spec[base + i] = m + sd;
                    r->spec[base + i] = m - sd;
                }
            }
        }
        window += l->group_len[g];
    }
}

/* Intensity stereo (section 4.6.8.2): in an intensity band, the right
 * channel is the left channel scaled by 0.5^(position / 4). The sign is
 * inverted for codebook 14, and inverted again where the M/S mask is set. */
static void apply_intensity(struct aac_decoder *d, struct ics *l, struct ics *r, unsigned ms_mask_present)
{
    unsigned window = 0;
    for (unsigned g = 0; g < r->num_groups; g++) {
        for (unsigned k = 0; k < r->max_sfb; k++) {
            unsigned cb = r->cb[g][k];
            if (cb != INTENSITY_HCB && cb != INTENSITY_HCB2)
                continue;
            float scale = pow2_quarter(-r->sf[g][k]);
            if (cb == INTENSITY_HCB2)
                scale = -scale;
            if (ms_mask_present == 1 && d->ms_used[g][k])
                scale = -scale;
            for (unsigned w = 0; w < r->group_len[g]; w++) {
                unsigned base = (window + w) * AAC_SHORT;
                for (unsigned i = r->offsets[k]; i < r->offsets[k + 1]; i++)
                    r->spec[base + i] = l->spec[base + i] * scale;
            }
        }
        window += r->group_len[g];
    }
}

/* Temporal noise shaping (section 4.6.9). The function inverse quantises
 * the coefficients of each filter and converts them from reflection to
 * direct-form coefficients. It then runs each filter as an all-pole filter
 * over its bands, upwards or downwards in frequency. */
static void apply_tns(struct ics *s)
{
    for (unsigned w = 0; w < s->num_windows; w++) {
        float *x = s->spec + w * AAC_SHORT;
        unsigned bottom = s->num_swb;
        for (unsigned f = 0; f < s->tns_filters[w]; f++) {
            const struct tns_filter *t = &s->tns[w][f];
            unsigned top = bottom;
            bottom = t->length > top ? 0 : top - t->length;
            if (!t->order)
                continue;
            double iq = ((1 << (t->coef_res - 1)) - 0.5) / (M_PI / 2);
            double iq_neg = ((1 << (t->coef_res - 1)) + 0.5) / (M_PI / 2);
            double refl[20], a[21] = { 1 }, tmp[21];
            for (unsigned i = 0; i < t->order; i++)
                refl[i] = sin(t->coef[i] / (t->coef[i] >= 0 ? iq : iq_neg));
            for (unsigned m = 1; m <= t->order; m++) {
                for (unsigned i = 1; i < m; i++)
                    tmp[i] = a[i] + refl[m - 1] * a[m - i];
                for (unsigned i = 1; i < m; i++)
                    a[i] = tmp[i];
                a[m] = refl[m - 1];
            }
            unsigned limit = s->tns_max < s->max_sfb ? s->tns_max : s->max_sfb;
            unsigned lo = s->offsets[bottom < limit ? bottom : limit];
            unsigned hi = s->offsets[top < limit ? top : limit];
            if (hi <= lo)
                continue;
            unsigned size = hi - lo;
            int step = t->direction ? -1 : 1;
            int start = t->direction ? (int)hi - 1 : (int)lo;
            /* y[n] = x[n] - sum of a[i] y[n - i] over the filtered range. */
            for (unsigned n = 0; n < size; n++) {
                int at = start + step * (int)n;
                double y = x[at];
                for (unsigned i = 1; i <= t->order && i <= n; i++)
                    y -= a[i] * x[at - step * (int)i];
                x[at] = (float)y;
            }
        }
    }
}

/* ---- elements ---- */

static void finish_channel(struct aac_decoder *d, struct ics *s, unsigned channel)
{
    if (s->tns_present)
        apply_tns(s);
    aac_filterbank_apply(&d->fb, &d->ch[channel], s->spec, s->sequence, s->shape, d->pcm[channel]);
}

static int decode_single(struct aac_decoder *d, struct aac_bits *b, unsigned channel)
{
    struct ics *s = &d->ics[0];
    aac_skip(b, 4);                     /* element_instance_tag */
    int err = read_ics(d, b, s, 0);
    if (err)
        return err;
    memset(d->ms_used, 0, sizeof d->ms_used);
    reconstruct(d, s, NULL);
    finish_channel(d, s, channel);
    return 0;
}

static int decode_pair(struct aac_decoder *d, struct aac_bits *b, unsigned channel)
{
    struct ics *l = &d->ics[0], *r = &d->ics[1];
    aac_skip(b, 4);                     /* element_instance_tag */
    int common = aac_bit(b), err;
    unsigned ms_mask_present = 0;
    memset(d->ms_used, 0, sizeof d->ms_used);
    if (common) {
        if ((err = read_ics_info(d, b, l)) < 0)
            return err;
        ms_mask_present = aac_get(b, 2);
        if (ms_mask_present == 3)
            return -EBADMSG;
        for (unsigned g = 0; g < l->num_groups; g++)
            for (unsigned k = 0; k < l->max_sfb; k++)
                d->ms_used[g][k] = ms_mask_present == 2 ? 1 : ms_mask_present == 1 ? (uint8_t)aac_bit(b) : 0;
        r->sequence = l->sequence;
        r->shape = l->shape;
        r->max_sfb = l->max_sfb;
        r->num_windows = l->num_windows;
        r->num_groups = l->num_groups;
        memcpy(r->group_len, l->group_len, sizeof r->group_len);
        r->offsets = l->offsets;
        r->num_swb = l->num_swb;
        r->tns_max = l->tns_max;
    }
    if ((err = read_ics(d, b, l, common)) < 0 || (err = read_ics(d, b, r, common)) < 0)
        return err;
    reconstruct(d, l, NULL);
    reconstruct(d, r, common ? l : NULL);
    if (common) {
        apply_ms(d, l, r);
        apply_intensity(d, l, r, ms_mask_present);
    }
    finish_channel(d, l, channel);
    finish_channel(d, r, channel + 1);
    return 0;
}

/* data_stream_element (section 4.4.2.1): skipped. */
static void skip_dse(struct aac_bits *b, size_t start)
{
    aac_skip(b, 4);                     /* element_instance_tag */
    int align = aac_bit(b);
    unsigned count = aac_get(b, 8);
    if (count == 255)
        count += aac_get(b, 8);
    if (align)
        aac_align(b, start);
    aac_skip(b, 8 * (size_t)count);
}

/* fill_element (section 4.4.2.1): skipped, including SBR data. */
static void skip_fil(struct aac_bits *b)
{
    unsigned count = aac_get(b, 4);
    if (count == 15)
        count += aac_get(b, 8) - 1;
    aac_skip(b, 8 * (size_t)count);
}

int aac_decode_block(struct aac_decoder *d, struct aac_bits *b, size_t start, float *out)
{
    unsigned channel = 0, element = 0;
    for (;;) {
        unsigned id = aac_get(b, 3);
        if (b->overrun)
            return -EBADMSG;
        int err = 0;
        switch (id) {
        case AAC_SCE:
        case AAC_LFE:
        case AAC_CPE: {
            unsigned n = id == AAC_CPE ? 2 : 1;
            if (element >= d->cfg.nelements || channel + n > d->cfg.channels)
                return -EBADMSG;
            element++;
            err = id == AAC_CPE ? decode_pair(d, b, channel) : decode_single(d, b, channel);
            channel += n;
            break;
        }
        case AAC_CCE:
            return -ENOTSUP;
        case AAC_DSE:
            skip_dse(b, start);
            break;
        case AAC_PCE: {
            struct aac_config ignored = d->cfg;
            err = aac_parse_pce(b, start, &ignored);
            break;
        }
        case AAC_FIL:
            skip_fil(b);
            break;
        case AAC_END:
            aac_align(b, start);
            if (channel != d->cfg.channels)
                return -EBADMSG;
            for (unsigned n = 0; n < AAC_FRAME; n++)
                for (unsigned c = 0; c < d->cfg.channels; c++) {
                    float v = d->pcm[d->cfg.order[c]][n] * (1.0f / 32768.0f);
                    out[n * d->cfg.channels + c] = v;
                }
            return b->overrun ? -EBADMSG : 0;
        }
        if (err)
            return err;
        if (b->overrun)
            return -EBADMSG;
    }
}
