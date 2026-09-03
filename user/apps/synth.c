/* synth: a small monophonic subtractive synthesizer for audiod.
 *
 * Signal path (synthvoice.h): oscillator (saw, square or triangle,
 * octave shifted) -> ADSR amplitude envelope -> resonant state-variable
 * low-pass filter whose cutoff follows the envelope -> soft clipper
 * -> audiod stream.
 *
 * Audio runs from the application event loop: whenever the audiod
 * connection becomes readable, every buffer the server has handed back is
 * rendered and queued again, so the server always holds a full pool and a
 * late wakeup of up to three periods does not cause an xrun. */
#include <audio/audio.h>
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include "synthvoice.h"

#define NOTES 13
#define SCOPE_EVERY 6           /* periods between oscilloscope repaints */

static struct app *app;
static struct audio_connection *audio;
static struct audio_playback *playback;
static struct watch *audio_watch;
static struct widget *wave, *octave, *cutoff, *resonance, *envelope_amount;
static struct widget *volume, *attack, *decay, *sustain, *release;
static struct widget *cutoff_label, *resonance_label, *envelope_label;
static struct widget *volume_label, *attack_label, *decay_label;
static struct widget *sustain_label, *release_label;
static struct widget *status, *keys, *scope;
static int16_t *period;
static uint32_t quantum;
static struct synth_voice voice;
static int active_note = -1, active_code, gate, mouse_gate;
static uint32_t shown_xruns, periods_rendered;

static const int frequencies[NOTES] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494, 523
};
static const char *const note_names[NOTES] = {
    "C", "C#", "D", "D#", "E", "F", "F#",
    "G", "G#", "A", "A#", "B", "C"
};
static const int note_codes[NOTES] = {
    0x1e, 0x11, 0x1f, 0x12, 0x20, 0x21, 0x14,
    0x22, 0x15, 0x23, 0x16, 0x24, 0x25
};

/* The frequency of a pad at the current octave setting. */
static float note_frequency(int note)
{
    return (float)frequencies[note] * (float)(1 << (octave->value + 2)) / 4.0f;
}

static void update_status(void)
{
    char text[128];
    uint32_t xruns = audio_playback_xruns(playback);
    if (active_note >= 0)
        snprintf(text, sizeof text, "%s%d  %d Hz    xruns %u",
                 note_names[active_note],
                 4 + octave->value + (active_note == NOTES - 1),
                 frequencies[active_note] << (octave->value + 2) >> 2,
                 xruns);
    else
        snprintf(text, sizeof text,
                 "Pads or A W S E D F T G Y H U J K, Z/X octave    xruns %u",
                 xruns);
    widget_set_text(status, text);
}

static void note_on(int note, int code, int mouse)
{
    if (note < 0 || note >= NOTES)
        return;
    active_note = note;
    active_code = code;
    mouse_gate = mouse;
    gate = 1;
    synth_voice_on(&voice, note_frequency(note));
    update_status();
    widget_invalidate(keys);
}

static void note_off(int code, int mouse)
{
    if (!gate || mouse_gate != mouse || (!mouse && active_code != code))
        return;
    gate = 0;
    active_code = 0;
    mouse_gate = 0;
    synth_voice_off(&voice);
    widget_invalidate(keys);
}

static void render_period(void)
{
    struct synth_params params = {
        .wave = (enum synth_wave)wave->value,
        .cutoff = (float)cutoff->value,
        .resonance = resonance->value / 100.0f,
        .envelope_amount = envelope_amount->value / 100.0f,
        .attack_ms = (float)attack->value,
        .decay_ms = (float)decay->value,
        .release_ms = (float)release->value,
        .sustain = sustain->value / 100.0f,
    };
    struct synth_coeffs coeffs;
    synth_coeffs_set(&coeffs, &params);
    int was_active = synth_voice_active(&voice);
    for (uint32_t i = 0; i < quantum; i++) {
        int16_t pcm = (int16_t)(synth_voice_sample(&voice, &coeffs) * 24000.0f);
        period[i * 2] = pcm;
        period[i * 2 + 1] = pcm;
    }
    if (was_active && !synth_voice_active(&voice))
        widget_invalidate(keys);        /* the release ended: the pad goes quiet */
    if (++periods_rendered % SCOPE_EVERY == 0)
        widget_invalidate(scope);
}

/* Render into every buffer the server has handed back. */
static int fill_ready_buffers(void)
{
    while (audio_playback_ready(playback) > 0) {
        render_period();
        if (audio_playback_write(playback, period, quantum) != (ssize_t)quantum)
            return -1;
    }
    return 0;
}

static void audio_event(int fd, int revents, void *arg)
{
    if (revents & (POLLERR | POLLHUP | POLLNVAL) ||
        audio_connection_dispatch(audio, 1) < 0 || fill_ready_buffers() < 0) {
        widget_set_text(status, "The audio server disconnected");
        if (audio_watch) {
            app_unwatch_fd(app, audio_watch);
            audio_watch = NULL;
        }
        return;
    }
    uint32_t xruns = audio_playback_xruns(playback);
    if (xruns != shown_xruns) {
        shown_xruns = xruns;
        update_status();
    }
}

static void update_controls(void)
{
    char text[48];
    snprintf(text, sizeof text, "Cutoff: %d Hz", cutoff->value);
    widget_set_text(cutoff_label, text);
    snprintf(text, sizeof text, "Resonance: %d%%", resonance->value);
    widget_set_text(resonance_label, text);
    snprintf(text, sizeof text, "Env to cutoff: %d%%", envelope_amount->value);
    widget_set_text(envelope_label, text);
    snprintf(text, sizeof text, "Volume: %d%%", volume->value);
    widget_set_text(volume_label, text);
    snprintf(text, sizeof text, "Attack: %d ms", attack->value);
    widget_set_text(attack_label, text);
    snprintf(text, sizeof text, "Decay: %d ms", decay->value);
    widget_set_text(decay_label, text);
    snprintf(text, sizeof text, "Sustain: %d%%", sustain->value);
    widget_set_text(sustain_label, text);
    snprintf(text, sizeof text, "Release: %d ms", release->value);
    widget_set_text(release_label, text);
}

static int on_control(struct widget *w, void *args, void *arg)
{
    if (w == volume)
        audio_playback_set_volume(playback, (unsigned)volume->value);
    if (w == octave) {
        if (active_note >= 0 && synth_voice_active(&voice))
            voice.increment = note_frequency(active_note) / SYNTH_RATE;
        update_status();
    }
    update_controls();
    return 1;
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    int width = w->w / NOTES;
    for (int i = 0; i < NOTES; i++) {
        int x = i * width;
        int right = i == NOTES - 1 ? w->w : x + width;
        uint32_t fill = i == active_note && synth_voice_active(&voice) ?
                        t->color[TC_ACCENT] : t->color[TC_FIELD];
        painter_fill(p, x + 1, 1, right - x - 2, w->h - 2, fill);
        painter_frame(p, x, 0, right - x, w->h, t->color[TC_BORDER]);
        int tw = painter_text_width(p, note_names[i], -1);
        painter_text(p, x + (right - x - tw) / 2,
                     (w->h - painter_text_height(p)) / 2,
                     note_names[i], t->color[TC_TEXT]);
    }
    return 1;
}

/* The most recently rendered period, one trace across the canvas. */
static int on_paint_scope(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[TC_BORDER]);
    int mid = w->h / 2;
    painter_line(p, 1, mid, w->w - 2, mid, t->color[TC_BORDER]);
    if (!period || quantum == 0 || w->w < 4)
        return 1;
    int scale = mid - 2;
    int prev_x = 1, prev_y = mid - period[0] * scale / 24000;
    for (int x = 2; x < w->w - 1; x++) {
        uint32_t i = (uint32_t)(x - 1) * (quantum - 1) / (uint32_t)(w->w - 3);
        int y = mid - period[i * 2] * scale / 24000;
        painter_line(p, prev_x, prev_y, x, y, t->color[TC_ACCENT]);
        prev_x = x;
        prev_y = y;
    }
    return 1;
}

static int note_at(struct widget *w, int x)
{
    if (x < 0 || x >= w->w)
        return -1;
    int note = x * NOTES / w->w;
    return note < NOTES ? note : NOTES - 1;
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *click = args;
    if (!(click->button & 1))
        return 0;
    note_on(note_at(w, click->x), 0, 1);
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *click = args;
    if (!(click->button & 1) || !mouse_gate)
        return 0;
    int note = note_at(w, click->x);
    if (note >= 0 && note != active_note)
        note_on(note, 0, 1);
    return 1;
}

static int on_release(struct widget *w, void *args, void *arg)
{
    note_off(0, 1);
    return 1;
}

static void shift_octave(int delta)
{
    int value = octave->value + delta;
    if (value < octave->min || value > octave->max)
        return;
    widget_set_value(octave, value);
    if (active_note >= 0 && synth_voice_active(&voice))
        voice.increment = note_frequency(active_note) / SYNTH_RATE;
    update_status();
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *key = args;
    for (int i = 0; i < NOTES; i++)
        if (key->code == note_codes[i]) {
            if (!gate || active_code != key->code || mouse_gate)
                note_on(i, key->code, 0);
            return 1;
        }
    switch (key->code) {
    case 0x2c:                  /* Z */
        shift_octave(-1);
        return 1;
    case 0x2d:                  /* X */
        shift_octave(1);
        return 1;
    case 0x01:                  /* Escape */
        app_quit(app, 0);
        return 1;
    }
    return 0;
}

static int on_keyup(struct widget *w, void *args, void *arg)
{
    struct sig_key *key = args;
    note_off(key->code, 0);
    return 1;
}

static struct widget *add_slider(struct widget *grid, int row, int column,
                                 struct widget **label, int min, int max,
                                 int value)
{
    *label = label_new(grid, "");
    widget_set_grid(*label, row, column * 2, 1, 1);
    struct widget *slider = slider_new(grid, min, max, value);
    widget_set_grid(slider, row, column * 2 + 1, 1, 1);
    widget_connect(slider, "changed", on_control, NULL);
    return slider;
}

int main(void)
{
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 620, 440, "synthesizer");
    if (!win)
        return 1;

    audio = audio_connect();
    playback = audio ? audio_playback_create(audio, "synthesizer") : NULL;
    if (!playback) {
        static const char *const buttons[] = { "Close" };
        app_dialog(app, "Synthesizer", "The desktop audio service is unavailable.", buttons, 1);
        audio_disconnect(audio);
        app_destroy(app);
        return 1;
    }
    quantum = audio_playback_quantum(playback);
    period = calloc((size_t)quantum * 2, sizeof *period);
    if (!period) {
        audio_disconnect(audio);
        app_destroy(app);
        return 1;
    }

    struct widget *controls = grid_new(win);
    widget_set_stretch(controls, 1, 0);
    grid_set_stretch(controls, -1, 1, 1);
    grid_set_stretch(controls, -1, 3, 1);
    widget_set_grid(label_new(controls, "Oscillator"), 0, 0, 1, 1);
    wave = combobox_new(controls);
    combobox_add(wave, "Saw");
    combobox_add(wave, "Square");
    combobox_add(wave, "Triangle");
    combobox_select(wave, 0);
    widget_set_grid(wave, 0, 1, 1, 1);
    widget_set_grid(label_new(controls, "Octave"), 0, 2, 1, 1);
    octave = spinner_new(controls, -2, 2, 0);
    widget_set_grid(octave, 0, 3, 1, 1);
    widget_connect(octave, "changed", on_control, NULL);
    cutoff = add_slider(controls, 1, 0, &cutoff_label, 100, 8000, 1800);
    resonance = add_slider(controls, 1, 1, &resonance_label, 0, 90, 35);
    envelope_amount = add_slider(controls, 2, 0, &envelope_label, 0, 100, 40);
    volume = add_slider(controls, 2, 1, &volume_label, 0, 100, 100);
    attack = add_slider(controls, 3, 0, &attack_label, 1, 500, 15);
    decay = add_slider(controls, 3, 1, &decay_label, 1, 1000, 200);
    sustain = add_slider(controls, 4, 0, &sustain_label, 0, 100, 70);
    release = add_slider(controls, 4, 1, &release_label, 10, 1500, 280);
    update_controls();

    scope = canvas_new(win);
    widget_set_hint(scope, 0, 70);
    widget_connect(scope, "paint", on_paint_scope, NULL);

    keys = canvas_new(win);
    widget_set_hint(keys, 0, 100);
    widget_connect(keys, "paint", on_paint, NULL);
    widget_connect(keys, "press", on_press, NULL);
    widget_connect(keys, "motion", on_motion, NULL);
    widget_connect(keys, "release", on_release, NULL);
    widget_connect(keys, "key", on_key, NULL);
    widget_connect(keys, "keyup", on_keyup, NULL);
    widget_focus(keys);
    status = label_new(win, "");
    update_status();

    /* Pre-roll the whole pool before the stream starts. */
    if (fill_ready_buffers() < 0 || audio_playback_start(playback) < 0) {
        free(period);
        audio_disconnect(audio);
        app_destroy(app);
        return 1;
    }
    audio_watch = app_watch_fd(app, audio_connection_fd(audio), POLLIN,
                               audio_event, NULL);
    int code = app_run(app);
    if (audio_watch)
        app_unwatch_fd(app, audio_watch);
    audio_playback_pause(playback);
    free(period);
    audio_disconnect(audio);
    app_destroy(app);
    return code;
}
