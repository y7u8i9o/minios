/* The AAC filter bank (section 4.6.11 of ISO/IEC 14496-3). It computes the
 * inverse MDCT of one long window of 2048 samples or of eight short windows
 * of 256 samples. It applies the window shapes (sine or Kaiser-Bessel
 * derived) and the four window sequences, and it overlaps and adds the
 * result to the previous frame.
 *
 * As the standard requires, each window uses the shape of the previous
 * frame for its rising half and the shape of the current frame for its
 * falling half. */
#include "aac.h"
#include <math.h>
#include <string.h>

/* Computes the zeroth-order modified Bessel function of the first kind from
 * its power series. */
static double bessel_i0(double x)
{
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 50; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

/* Computes the rising half (half samples) of a Kaiser-Bessel derived window
 * with parameter alpha. Each sample is the square root of the normalised
 * running sum of a Kaiser kernel of half + 1 points. */
static void kbd_window(float *w, unsigned half, double alpha)
{
    double kernel[AAC_FRAME + 1], total = 0;
    for (unsigned n = 0; n <= half; n++) {
        double r = ((double)n - half / 2.0) / (half / 2.0);
        kernel[n] = bessel_i0(M_PI * alpha * sqrt(1.0 - r * r));
        total += kernel[n];
    }
    double sum = 0;
    for (unsigned n = 0; n < half; n++) {
        sum += kernel[n];
        w[n] = (float)sqrt(sum / total);
    }
}

static void sine_window(float *w, unsigned half)
{
    for (unsigned n = 0; n < half; n++)
        w[n] = (float)sin(M_PI / (2.0 * half) * (n + 0.5));
}

int aac_filterbank_init(struct aac_filterbank *f)
{
    memset(f, 0, sizeof *f);
    int err = codec_mdct_init(&f->long_mdct, 2 * AAC_FRAME);
    if (!err)
        err = codec_mdct_init(&f->short_mdct, 2 * AAC_SHORT);
    if (err) {
        aac_filterbank_free(f);
        return err;
    }
    sine_window(f->sine_long, AAC_FRAME);
    sine_window(f->sine_short, AAC_SHORT);
    kbd_window(f->kbd_long, AAC_FRAME, 4);
    kbd_window(f->kbd_short, AAC_SHORT, 6);
    return 0;
}

void aac_filterbank_free(struct aac_filterbank *f)
{
    codec_mdct_free(&f->long_mdct);
    codec_mdct_free(&f->short_mdct);
}

static const float *long_window(const struct aac_filterbank *f, unsigned shape)
{
    return shape ? f->kbd_long : f->sine_long;
}

static const float *short_window(const struct aac_filterbank *f, unsigned shape)
{
    return shape ? f->kbd_short : f->sine_short;
}

void aac_filterbank_apply(struct aac_filterbank *f, struct aac_channel *ch, const float *spec, unsigned sequence,
                          unsigned shape, float *out)
{
    float *t = f->time;
    const unsigned N = 2 * AAC_FRAME, S = AAC_SHORT;
    /* The standard scales the inverse transform by 2 / N. codec_imdct does not
     * scale, so this function applies the factor. */
    if (sequence == AAC_EIGHT_SHORT) {
        memset(t, 0, sizeof f->time);
        const float *prev = short_window(f, ch->prev_shape), *cur = short_window(f, shape);
        for (unsigned w = 0; w < 8; w++) {
            float *blk = f->block;
            codec_imdct(&f->short_mdct, spec + w * S, blk);
            const float *rise = w == 0 ? prev : cur;
            /* The eight windows overlap by half. They start 448 samples into
             * the frame and are centred on its middle. */
            float *dst = t + 448 + w * S;
            for (unsigned n = 0; n < S; n++) {
                dst[n] += blk[n] * rise[n] * (2.0f / (2 * S));
                dst[S + n] += blk[S + n] * cur[S - 1 - n] * (2.0f / (2 * S));
            }
        }
    } else {
        codec_imdct(&f->long_mdct, spec, t);
        const float scale = 2.0f / N;
        for (unsigned n = 0; n < N; n++)
            t[n] *= scale;
        if (sequence == AAC_LONG_STOP) {
            /* Short rising slope after a short block. */
            const float *rise = short_window(f, ch->prev_shape);
            for (unsigned n = 0; n < 448; n++)
                t[n] = 0;
            for (unsigned n = 0; n < S; n++)
                t[448 + n] *= rise[n];
        } else {
            const float *rise = long_window(f, ch->prev_shape);
            for (unsigned n = 0; n < AAC_FRAME; n++)
                t[n] *= rise[n];
        }
        if (sequence == AAC_LONG_START) {
            /* Short falling slope before a short block. */
            const float *fall = short_window(f, shape);
            for (unsigned n = 0; n < S; n++)
                t[AAC_FRAME + 448 + n] *= fall[S - 1 - n];
            for (unsigned n = AAC_FRAME + 448 + S; n < N; n++)
                t[n] = 0;
        } else {
            const float *fall = long_window(f, shape);
            for (unsigned n = 0; n < AAC_FRAME; n++)
                t[AAC_FRAME + n] *= fall[AAC_FRAME - 1 - n];
        }
    }
    for (unsigned n = 0; n < AAC_FRAME; n++)
        out[n] = t[n] + ch->overlap[n];
    memcpy(ch->overlap, t + AAC_FRAME, sizeof ch->overlap);
    ch->prev_shape = shape;
}
