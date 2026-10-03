#pragma once
/* One subtractive synthesizer voice, shared by the synthesizer and the
 * step sequencer: an oscillator (saw, square or triangle) feeds a
 * linear ADSR amplitude envelope and a resonant state-variable low-pass
 * filter whose cutoff follows the envelope; a cubic soft clipper
 * follows.  The filter runs twice per sample at half the coefficient so
 * that it remains stable up to the highest cutoff the envelope sweeps to.
 *
 * The functions are static: every program includes this header once. */
#include <math.h>
#include <stdint.h>

#define SYNTH_RATE 48000
#define SYNTH_OVERSAMPLE 2

enum synth_wave { SYNTH_SAW, SYNTH_SQUARE, SYNTH_TRIANGLE };
enum synth_stage { SYNTH_IDLE, SYNTH_ATTACK, SYNTH_DECAY, SYNTH_SUSTAIN, SYNTH_RELEASE };

/* User facing settings. */
struct synth_params {
    enum synth_wave wave;
    float cutoff;               /* Hz */
    float resonance;            /* 0 to 1 */
    float envelope_amount;      /* 0 to 1: how far the envelope sweeps the cutoff */
    float attack_ms, decay_ms, release_ms;
    float sustain;              /* 0 to 1 */
};

/* Per sample coefficients derived from the parameters. */
struct synth_coeffs {
    enum synth_wave wave;
    float attack_step, decay_step, release_step, sustain_level;
    float base_cutoff, sweep, damping;
};

struct synth_voice {
    float phase, increment;
    float envelope;
    float low, band;
    enum synth_stage stage;
};

static inline void synth_coeffs_set(struct synth_coeffs *c, const struct synth_params *p)
{
    c->wave = p->wave;
    c->attack_step = 1.0f / ((p->attack_ms < 1.0f ? 1.0f : p->attack_ms) * (SYNTH_RATE / 1000.0f));
    c->decay_step = 1.0f / ((p->decay_ms < 1.0f ? 1.0f : p->decay_ms) * (SYNTH_RATE / 1000.0f));
    c->release_step = 1.0f / ((p->release_ms < 1.0f ? 1.0f : p->release_ms) * (SYNTH_RATE / 1000.0f));
    c->sustain_level = p->sustain;
    c->base_cutoff = p->cutoff;
    c->sweep = p->envelope_amount * 6000.0f;
    c->damping = 1.9f - 1.6f * p->resonance;
}

/* Start a note.  The waveform restarts only when nothing is sounding, so
 * a retrigger or a legato change is free of clicks. */
static inline void synth_voice_on(struct synth_voice *v, float frequency)
{
    if (v->stage == SYNTH_IDLE)
        v->phase = 0.0f;
    v->increment = frequency / SYNTH_RATE;
    v->stage = SYNTH_ATTACK;
}

static inline void synth_voice_off(struct synth_voice *v)
{
    if (v->stage != SYNTH_IDLE)
        v->stage = SYNTH_RELEASE;
}

static inline int synth_voice_active(const struct synth_voice *v)
{
    return v->stage != SYNTH_IDLE;
}

static inline float synth_oscillator(struct synth_voice *v, enum synth_wave wave)
{
    float sample;
    if (wave == SYNTH_SQUARE)
        sample = v->phase < 0.5f ? 1.0f : -1.0f;
    else if (wave == SYNTH_TRIANGLE)
        sample = 1.0f - 4.0f * fabsf(v->phase - 0.5f);
    else
        sample = v->phase * 2.0f - 1.0f;
    v->phase += v->increment;
    if (v->phase >= 1.0f)
        v->phase -= 1.0f;
    return sample;
}

static inline float synth_envelope(struct synth_voice *v, const struct synth_coeffs *c)
{
    switch (v->stage) {
    case SYNTH_ATTACK:
        v->envelope += c->attack_step;
        if (v->envelope >= 1.0f) {
            v->envelope = 1.0f;
            v->stage = SYNTH_DECAY;
        }
        break;
    case SYNTH_DECAY:
        v->envelope -= c->decay_step;
        if (v->envelope <= c->sustain_level) {
            v->envelope = c->sustain_level;
            v->stage = SYNTH_SUSTAIN;
        }
        break;
    case SYNTH_SUSTAIN:
        v->envelope = c->sustain_level;
        break;
    case SYNTH_RELEASE:
        v->envelope -= c->release_step;
        if (v->envelope <= 0.0f) {
            v->envelope = 0.0f;
            v->stage = SYNTH_IDLE;
        }
        break;
    case SYNTH_IDLE:
        v->envelope = 0.0f;
        break;
    }
    return v->envelope;
}

/* Cubic saturation: transparent for small signals, no hard edge at full
 * scale.  The filter can overshoot at high resonance. */
static inline float synth_soft_clip(float x)
{
    if (x > 1.5f)
        x = 1.5f;
    else if (x < -1.5f)
        x = -1.5f;
    return x - x * x * x * (4.0f / 27.0f);
}

/* One output sample in about -1 to 1. */
static inline float synth_voice_sample(struct synth_voice *v, const struct synth_coeffs *c)
{
    float level = synth_envelope(v, c);
    float input = synth_oscillator(v, c->wave) * level * 0.6f;
    float fc = c->base_cutoff + c->sweep * level;
    if (fc > 12000.0f)
        fc = 12000.0f;
    float f = (float)M_PI * fc / (SYNTH_OVERSAMPLE * SYNTH_RATE);
    for (int k = 0; k < SYNTH_OVERSAMPLE; k++) {
        v->low += f * v->band;
        float high = input - v->low - c->damping * v->band;
        v->band += f * high;
    }
    return synth_soft_clip(v->low);
}
