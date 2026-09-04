/* player: a WAV player with a waveform view.
 *
 * The file (PCM, 8 or 16 bit, mono or stereo, any rate) is decoded to
 * the 48 kHz stereo format of the audio server when it is opened, with
 * linear interpolation for other rates.  The waveform view shows the
 * whole file as one column of minimum and maximum per pixel and a play
 * head; a click seeks.  Audio runs from the application event loop:
 * every buffer the server hands back is refilled at once. */
#include <audio/audio.h>
#include <gui/app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>

#define RATE 48000

static struct app *app;
static struct audio_connection *audio;
static struct audio_playback *playback;
static struct watch *audio_watch;
static struct timer *tick;
static struct widget *wave, *play_button, *loop_box, *volume, *volume_label, *info, *time_label;
static int16_t *samples;            /* decoded, interleaved stereo at 48 kHz */
static uint32_t frames, position;   /* in frames */
static int16_t *period;
static uint32_t quantum;
static int playing, loop;
static char file_name[128];
static char format_text[64];
static int *column_min, *column_max, columns;

/* ---- WAV decoding ---- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* One sample of the file as a 16-bit value: 8-bit unsigned, 16, 24 and
 * 32-bit signed little endian, the wider ones by their top 16 bits. */
static int16_t file_sample(const uint8_t *data, uint32_t index, int bits)
{
    const uint8_t *p = data + (size_t)index * (bits / 8);
    switch (bits) {
    case 8: return (int16_t)((p[0] - 128) << 8);
    case 24: return (int16_t)(p[1] | p[2] << 8);
    case 32: return (int16_t)(p[2] | p[3] << 8);
    default: return (int16_t)le16(p);
    }
}

static int decode(const uint8_t *file, size_t size)
{
    if (size < 12 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0)
        return -1;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    uint32_t data_size = 0;
    size_t at = 12;
    while (at + 8 <= size) {
        uint32_t len = le32(file + at + 4);
        const uint8_t *body = file + at + 8;
        if (len > size - at - 8)
            len = (uint32_t)(size - at - 8);
        if (memcmp(file + at, "fmt ", 4) == 0 && len >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
        } else if (memcmp(file + at, "data", 4) == 0) {
            data = body;
            data_size = len;
        }
        at += 8 + len + (len & 1);
    }
    if ((format != 1 && format != 0xfffe) || !data || rate == 0 ||
        (channels != 1 && channels != 2) || (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return -1;
    uint32_t frame_bytes = channels * bits / 8;
    uint32_t in_frames = data_size / frame_bytes;
    if (in_frames < 2)
        return -1;
    uint64_t out_frames = (uint64_t)in_frames * RATE / rate;
    if (out_frames > 64u << 20)
        return -1;
    int16_t *out = malloc((size_t)out_frames * 2 * sizeof *out);
    if (!out)
        return -1;
    for (uint64_t i = 0; i < out_frames; i++) {
        /* The source position of output frame i and its fraction, in
         * 16.16 fixed point, for linear interpolation. */
        uint64_t pos = i * rate * 65536 / RATE;
        uint32_t src = (uint32_t)(pos >> 16), frac = (uint32_t)(pos & 0xffff);
        uint32_t next = src + 1 < in_frames ? src + 1 : src;
        for (int c = 0; c < 2; c++) {
            uint32_t ch = channels == 2 ? (uint32_t)c : 0;
            int32_t a = file_sample(data, src * channels + ch, bits);
            int32_t b = file_sample(data, next * channels + ch, bits);
            out[i * 2 + c] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
        }
    }
    free(samples);
    samples = out;
    frames = (uint32_t)out_frames;
    snprintf(format_text, sizeof format_text, "%u Hz %s %u-bit", rate,
             channels == 2 ? "stereo" : "mono", bits);
    return 0;
}

static int load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    int r = decode(buf, (size_t)size);
    free(buf);
    if (r < 0)
        return -1;
    const char *base = strrchr(path, '/');
    snprintf(file_name, sizeof file_name, "%s", base ? base + 1 : path);
    position = 0;
    columns = 0;                    /* the overview is rebuilt on the next paint */
    return 0;
}

/* ---- audio ---- */

static void update_time(void)
{
    char text[64];
    uint32_t p = position / RATE, total = frames / RATE;
    snprintf(text, sizeof text, "%u:%02u / %u:%02u", p / 60, p % 60, total / 60, total % 60);
    widget_set_text(time_label, text);
}

static void set_playing(int on)
{
    if (on && !frames)
        return;
    if (on == playing)
        return;
    playing = on;
    widget_set_text(play_button, playing ? "Pause" : "Play");
    printf("player: %s %s\n", playing ? "playing" : "paused", file_name);
    fflush(stdout);
}

/* Fill one period from the file; silence past the end (or wrap). */
static void render_period(void)
{
    uint32_t i = 0;
    while (i < quantum) {
        if (!playing || position >= frames) {
            if (playing && loop && frames) {
                position = 0;
                continue;
            }
            if (playing) {
                playing = 0;
                widget_set_text(play_button, "Play");
                printf("player: finished %s\n", file_name);
                fflush(stdout);
            }
            memset(period + i * 2, 0, (quantum - i) * 2 * sizeof *period);
            break;
        }
        uint32_t n = frames - position;
        if (n > quantum - i)
            n = quantum - i;
        memcpy(period + i * 2, samples + (size_t)position * 2, n * 2 * sizeof *period);
        position += n;
        i += n;
    }
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
        widget_set_text(info, "The audio server disconnected");
        if (audio_watch) {
            app_unwatch_fd(app, audio_watch);
            audio_watch = NULL;
        }
    }
}

static void on_tick(void *arg)
{
    update_time();
    widget_invalidate(wave);
}

/* ---- the view ---- */

static void build_overview(int width)
{
    free(column_min);
    free(column_max);
    columns = width;
    column_min = calloc((size_t)width, sizeof *column_min);
    column_max = calloc((size_t)width, sizeof *column_max);
    if (!column_min || !column_max || !frames) {
        columns = 0;
        return;
    }
    for (int x = 0; x < width; x++) {
        uint64_t from = (uint64_t)x * frames / (uint64_t)width;
        uint64_t to = (uint64_t)(x + 1) * frames / (uint64_t)width;
        if (to <= from)
            to = from + 1;
        int lo = 32767, hi = -32768;
        for (uint64_t i = from; i < to && i < frames; i++) {
            int v = (samples[i * 2] + samples[i * 2 + 1]) / 2;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        column_min[x] = lo;
        column_max[x] = hi;
    }
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[TC_BORDER]);
    int inner = w->w - 2;
    if (inner < 2)
        return 1;
    if (frames && columns != inner)
        build_overview(inner);
    int mid = w->h / 2, scale = mid - 3;
    painter_line(p, 1, mid, w->w - 2, mid, t->color[TC_BORDER]);
    if (!frames) {
        const char *text = "No file";
        painter_text(p, (w->w - painter_text_width(p, text, -1)) / 2,
                     mid - painter_text_height(p) / 2, text, t->color[TC_TEXT_DISABLED]);
        return 1;
    }
    for (int x = 0; x < columns; x++) {
        int top = mid - column_max[x] * scale / 32767;
        int bottom = mid - column_min[x] * scale / 32767;
        if (bottom <= top)
            bottom = top + 1;
        painter_fill(p, 1 + x, top, 1, bottom - top, t->color[TC_ACCENT]);
    }
    int head = 1 + (int)((uint64_t)position * (uint64_t)inner / frames);
    painter_fill(p, head, 1, 2, w->h - 2, t->color[TC_TEXT]);
    return 1;
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *click = args;
    if (!(click->button & 1) || !frames || w->w < 3)
        return 0;
    int x = click->x - 1;
    if (x < 0) x = 0;
    if (x > w->w - 2) x = w->w - 2;
    position = (uint32_t)((uint64_t)x * frames / (uint64_t)(w->w - 2));
    update_time();
    widget_invalidate(w);
    return 1;
}

/* ---- controls ---- */

static void show_info(void)
{
    char text[256];
    if (frames)
        snprintf(text, sizeof text, "%s    %s    %u.%u s", file_name, format_text,
                 frames / RATE, (frames % RATE) * 10 / RATE);
    else
        snprintf(text, sizeof text, "No file loaded");
    widget_set_text(info, text);
    update_time();
}

static int on_play(struct widget *w, void *args, void *arg)
{
    if (!playing && frames && position >= frames)
        position = 0;
    set_playing(!playing);
    return 1;
}

static int on_stop(struct widget *w, void *args, void *arg)
{
    set_playing(0);
    position = 0;
    update_time();
    widget_invalidate(wave);
    return 1;
}

static int on_open(struct widget *w, void *args, void *arg)
{
    static char name[256] = "/usr/share/sounds/";
    static const char *const buttons[] = { "Close" };
    if (app_prompt(app, "Open", "File:", name, sizeof name)) {
        set_playing(0);
        if (load(name) < 0)
            app_dialog(app, "Error", "The file is not a PCM WAV file.", buttons, 1);
        show_info();
        widget_invalidate(wave);
    }
    return 1;
}

static int on_loop(struct widget *w, void *args, void *arg)
{
    loop = w->value;
    return 1;
}

static int on_volume(struct widget *w, void *args, void *arg)
{
    char text[32];
    snprintf(text, sizeof text, "Volume: %d%%", w->value);
    widget_set_text(volume_label, text);
    audio_playback_set_volume(playback, (unsigned)w->value);
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg)
{
    app_quit(app, 0);
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *key = args;
    if (key->code == 0x39) {        /* space */
        on_play(w, NULL, NULL);
        return 1;
    }
    if (key->code == 0x01) {
        app_quit(app, 0);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 560, 300, "player");
    if (!win)
        return 1;
    audio = audio_connect();
    playback = audio ? audio_playback_create(audio, "player") : NULL;
    if (!playback) {
        static const char *const buttons[] = { "Close" };
        app_dialog(app, "Player", "The desktop audio service is unavailable.", buttons, 1);
        audio_disconnect(audio);
        app_destroy(app);
        return 1;
    }
    quantum = audio_playback_quantum(playback);
    period = calloc((size_t)quantum * 2, sizeof *period);

    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, "File");
    widget_connect(menu_add(file, "Open...", "open"), "clicked", on_open, NULL);
    menu_add_separator(file);
    widget_connect(menu_add(file, "Quit", "quit"), "clicked", on_quit, NULL);

    struct widget *transport = box_new(win, 0);
    widget_set_stretch(transport, 1, 0);
    play_button = button_new(transport, "Play");
    widget_connect(play_button, "clicked", on_play, NULL);
    widget_connect(button_new(transport, "Stop"), "clicked", on_stop, NULL);
    loop_box = checkbox_new(transport, "Loop");
    widget_connect(loop_box, "toggled", on_loop, NULL);
    volume_label = label_new(transport, "Volume: 100%");
    volume = slider_new(transport, 0, 100, 100);
    widget_set_stretch(volume, 1, 0);
    widget_connect(volume, "changed", on_volume, NULL);
    time_label = label_new(transport, "0:00 / 0:00");

    wave = canvas_new(win);
    widget_set_stretch(wave, 1, 1);
    widget_set_hint(wave, 0, 120);
    widget_connect(wave, "paint", on_paint, NULL);
    widget_connect(wave, "press", on_press, NULL);
    widget_connect(wave, "key", on_key, NULL);
    widget_focus(wave);
    info = label_new(win, "");

    if (argc > 1) {
        if (load(argv[1]) < 0)
            fprintf(stderr, "player: cannot open %s\n", argv[1]);
        else
            set_playing(1);
    }
    show_info();

    if (fill_ready_buffers() < 0 || audio_playback_start(playback) < 0) {
        audio_disconnect(audio);
        app_destroy(app);
        return 1;
    }
    audio_watch = app_watch_fd(app, audio_connection_fd(audio), POLLIN, audio_event, NULL);
    tick = app_timer_add(app, 100, 1, on_tick, NULL);
    int code = app_run(app);
    if (audio_watch)
        app_unwatch_fd(app, audio_watch);
    audio_playback_pause(playback);
    audio_disconnect(audio);
    app_destroy(app);
    free(samples);
    free(period);
    return code;
}
