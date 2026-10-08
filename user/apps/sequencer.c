/* sequencer: a step sequencer on the synthesizer voice engine.
 *
 * Sixteen steps by eight notes of a major scale; every row is one voice
 * of synthvoice.h, so chords play polyphonically.  The transport runs
 * on the audio clock: the render loop counts frames and advances the
 * step, so the timing does not depend on the event loop.  Keys: space
 * plays and stops, D loads the demo pattern, C clears, Escape quits. */
#include <audio/audio.h>
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include "synthvoice.h"

#define STEPS 16
#define ROWS 8
#define STEPS_PER_BEAT 4

static struct app *app;
static struct audio_connection *audio;
static struct audio_playback *playback;
static struct watch *audio_watch;
static struct widget *grid, *play_button, *tempo, *octave, *wave;
static struct widget *cutoff, *resonance, *decay, *volume, *status;
static struct widget *cutoff_label, *resonance_label, *decay_label, *volume_label;
static int16_t *period;
static uint32_t quantum;
static uint8_t pattern[ROWS][STEPS];
static struct synth_voice voices[ROWS];
static int playing, step = -1, drawn_step = -1;
static uint32_t frames_left;        /* of the current step */
static int gate_open[ROWS];

/* Semitones of a major scale above the root, low row last. */
static const int scale[ROWS] = { 12, 11, 9, 7, 5, 4, 2, 0 };
static const char *const row_names[ROWS] = { "C", "B", "A", "G", "F", "E", "D", "C" };

static const uint8_t demo[ROWS][STEPS] = {
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0 },
    { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0 },
    { 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 },
    { 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
};

static float row_frequency(int row)
{
    /* C4 is 261.63 Hz; the octave spinner shifts by whole octaves. */
    float c = 261.63f * (float)(1 << (octave->value + 2)) / 16.0f;
    return c * powf(2.0f, scale[row] / 12.0f);
}

static uint32_t step_frames(void)
{
    return (uint32_t)(60.0f * SYNTH_RATE / ((float)tempo->value * STEPS_PER_BEAT));
}

static void update_status(void)
{
    char text[96];
    snprintf(text, sizeof text, "%s    %d bpm    xruns %u    Space play/stop, D demo, C clear",
             playing ? "Playing" : "Stopped", tempo->value, audio_playback_xruns(playback));
    widget_set_text(status, text);
}

/* Trigger the notes of a step; the gate closes halfway through it so
 * that repeated notes are heard as separate ones. */
static void start_step(int s)
{
    for (int row = 0; row < ROWS; row++) {
        if (pattern[row][s]) {
            synth_voice_on(&voices[row], row_frequency(row));
            gate_open[row] = 1;
        }
    }
}

static void close_gates(void)
{
    for (int row = 0; row < ROWS; row++)
        if (gate_open[row]) {
            synth_voice_off(&voices[row]);
            gate_open[row] = 0;
        }
}

static void render_period(void)
{
    struct synth_params params = {
        .wave = (enum synth_wave)wave->value,
        .cutoff = (float)cutoff->value,
        .resonance = resonance->value / 100.0f,
        .envelope_amount = 0.5f,
        .attack_ms = 4.0f,
        .decay_ms = (float)decay->value,
        .release_ms = (float)decay->value / 2.0f,
        .sustain = 0.0f,
    };
    struct synth_coeffs coeffs;
    synth_coeffs_set(&coeffs, &params);
    uint32_t length = step_frames();
    for (uint32_t i = 0; i < quantum; i++) {
        if (playing) {
            if (frames_left == 0) {
                step = (step + 1) % STEPS;
                frames_left = length;
                start_step(step);
            }
            if (frames_left == length / 2)
                close_gates();
            frames_left--;
        }
        float mix = 0.0f;
        for (int row = 0; row < ROWS; row++)
            if (synth_voice_active(&voices[row]))
                mix += synth_voice_sample(&voices[row], &coeffs);
        int16_t pcm = (int16_t)(synth_soft_clip(mix * 0.5f) * 24000.0f);
        period[i * 2] = pcm;
        period[i * 2 + 1] = pcm;
    }
    if (step != drawn_step)
        widget_invalidate(grid);
}

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
    }
}

static void set_playing(int on)
{
    if (on == playing)
        return;
    playing = on;
    if (on) {
        step = -1;
        frames_left = 0;
        printf("sequencer: playing %d bpm\n", tempo->value);
    } else {
        close_gates();
        step = -1;
        printf("sequencer: stopped\n");
    }
    fflush(stdout);
    widget_set_text(play_button, playing ? "Stop" : "Play");
    update_status();
    widget_invalidate(grid);
}

static int on_play(struct widget *w, void *args, void *arg)
{
    set_playing(!playing);
    widget_focus(grid);
    return 1;
}

static int on_clear(struct widget *w, void *args, void *arg)
{
    memset(pattern, 0, sizeof pattern);
    widget_invalidate(grid);
    widget_focus(grid);
    return 1;
}

static int on_demo(struct widget *w, void *args, void *arg)
{
    memcpy(pattern, demo, sizeof pattern);
    printf("sequencer: demo pattern loaded\n");
    fflush(stdout);
    widget_invalidate(grid);
    widget_focus(grid);
    return 1;
}

static void update_controls(void)
{
    char text[48];
    snprintf(text, sizeof text, "Cutoff: %d Hz", cutoff->value);
    widget_set_text(cutoff_label, text);
    snprintf(text, sizeof text, "Resonance: %d%%", resonance->value);
    widget_set_text(resonance_label, text);
    snprintf(text, sizeof text, "Decay: %d ms", decay->value);
    widget_set_text(decay_label, text);
    snprintf(text, sizeof text, "Volume: %d%%", volume->value);
    widget_set_text(volume_label, text);
}

static int on_control(struct widget *w, void *args, void *arg)
{
    if (w == volume)
        audio_playback_set_volume(playback, (unsigned)volume->value);
    update_controls();
    update_status();
    return 1;
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    int label_w = 24;
    int cw = (w->w - label_w) / STEPS, ch = w->h / ROWS;
    /* Every other beat is shaded; the playing step is a highlighted
     * column; a set cell is accent coloured. */
    for (int s = 0; s < STEPS; s++) {
        int x = label_w + s * cw;
        if (s == step && playing)
            painter_fill(p, x, 0, cw, ROWS * ch, t->color[TC_HIGHLIGHT]);
        else if ((s / STEPS_PER_BEAT) % 2)
            painter_fill(p, x, 0, cw, ROWS * ch, t->color[TC_WINDOW]);
    }
    for (int row = 0; row < ROWS; row++) {
        int y = row * ch;
        painter_text(p, 6, y + (ch - painter_text_height(p)) / 2, row_names[row], t->color[TC_TEXT]);
        for (int s = 0; s < STEPS; s++) {
            int x = label_w + s * cw;
            uint32_t fill = pattern[row][s] ? t->color[TC_ACCENT] : t->color[TC_FIELD];
            painter_rounded(p, x + 2, y + 2, cw - 4, ch - 4, fill, t->color[TC_BORDER]);
        }
    }
    painter_frame(p, 0, 0, w->w, w->h, t->color[TC_BORDER]);
    drawn_step = step;
    return 1;
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *click = args;
    if (!(click->button & 1))
        return 0;
    int label_w = 24;
    int cw = (w->w - label_w) / STEPS, ch = w->h / ROWS;
    if (click->x < label_w || cw < 1 || ch < 1)
        return 1;
    int s = (click->x - label_w) / cw, row = click->y / ch;
    if (s >= 0 && s < STEPS && row >= 0 && row < ROWS) {
        pattern[row][s] ^= 1;
        widget_invalidate(w);
    }
    widget_focus(w);
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *key = args;
    switch (key->code) {
    case 0x39:                  /* space */
        set_playing(!playing);
        return 1;
    case 0x20:                  /* D */
        on_demo(w, NULL, NULL);
        return 1;
    case 0x2e:                  /* C */
        on_clear(w, NULL, NULL);
        return 1;
    case 0x01:                  /* Escape */
        app_quit(app, 0);
        return 1;
    }
    return 0;
}

static struct widget *add_slider(struct widget *box, struct widget **label, int min, int max, int value)
{
    *label = label_new(box, "");
    struct widget *slider = slider_new(box, min, max, value);
    widget_connect(slider, "changed", on_control, NULL);
    return slider;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 640, 420, "sequencer");
    if (!win)
        return 1;
    audio = audio_connect();
    playback = audio ? audio_playback_create(audio, "sequencer") : NULL;
    if (!playback) {
        static const char *const buttons[] = { "Close" };
        app_dialog(app, "Sequencer", "The desktop audio service is unavailable.", buttons, 1);
        audio_disconnect(audio);
        app_destroy(app);
        return 1;
    }
    quantum = audio_playback_quantum(playback);
    period = calloc((size_t)quantum * 2, sizeof *period);
    if (!period)
        return 1;

    struct widget *transport = box_new(win, 0);
    widget_set_stretch(transport, 1, 0);
    play_button = button_new(transport, "Play");
    widget_connect(play_button, "clicked", on_play, NULL);
    widget_connect(button_new(transport, "Demo"), "clicked", on_demo, NULL);
    widget_connect(button_new(transport, "Clear"), "clicked", on_clear, NULL);
    label_new(transport, "Tempo");
    tempo = spinner_new(transport, 40, 240, 120);
    widget_connect(tempo, "changed", on_control, NULL);
    label_new(transport, "Octave");
    octave = spinner_new(transport, -2, 2, 0);
    widget_connect(octave, "changed", on_control, NULL);
    wave = combobox_new(transport);
    combobox_add(wave, "Saw");
    combobox_add(wave, "Square");
    combobox_add(wave, "Triangle");
    combobox_select(wave, 1);

    grid = canvas_new(win);
    grid->focusable = 1;
    widget_set_stretch(grid, 1, 1);
    widget_set_hint(grid, 0, 200);
    widget_connect(grid, "paint", on_paint, NULL);
    widget_connect(grid, "press", on_press, NULL);
    widget_connect(grid, "key", on_key, NULL);

    struct widget *controls = grid_new(win);
    widget_set_stretch(controls, 1, 0);
    grid_set_stretch(controls, -1, 1, 1);
    grid_set_stretch(controls, -1, 3, 1);
    cutoff = add_slider(controls, &cutoff_label, 100, 8000, 1200);
    widget_set_grid(cutoff_label, 0, 0, 1, 1);
    widget_set_grid(cutoff, 0, 1, 1, 1);
    resonance = add_slider(controls, &resonance_label, 0, 90, 40);
    widget_set_grid(resonance_label, 0, 2, 1, 1);
    widget_set_grid(resonance, 0, 3, 1, 1);
    decay = add_slider(controls, &decay_label, 20, 1000, 180);
    widget_set_grid(decay_label, 1, 0, 1, 1);
    widget_set_grid(decay, 1, 1, 1, 1);
    volume = add_slider(controls, &volume_label, 0, 100, 100);
    widget_set_grid(volume_label, 1, 2, 1, 1);
    widget_set_grid(volume, 1, 3, 1, 1);
    update_controls();
    status = label_new(win, "");
    update_status();
    widget_focus(grid);

    if (fill_ready_buffers() < 0 || audio_playback_start(playback) < 0) {
        audio_disconnect(audio);
        app_destroy(app);
        return 1;
    }
    audio_watch = app_watch_fd(app, audio_connection_fd(audio), POLLIN, audio_event, NULL);
    int code = app_run(app);
    if (audio_watch)
        app_unwatch_fd(app, audio_watch);
    audio_playback_pause(playback);
    free(period);
    audio_disconnect(audio);
    app_destroy(app);
    return code;
}
