/* The player program plays audio files and draws their waveform.
 *
 * The program reads every format a codec module of libcodec decodes
 * (docs/design/codecs.md), such as PCM WAV files with 8, 16, 24 or 32-bit
 * samples, at any sample rate.  Mono files play on both channels, and
 * files with more than two channels play their first two.  It converts
 * each file to the 48 kHz stereo format of the audio server when the
 * file is opened.  Linear
 * interpolation converts other sample rates by default.  The option -s and
 * the Resampling menu select an experimental windowed sinc resampler.  The
 * waveform view draws the minimum and the maximum of the samples in each
 * pixel column of the whole file, and a vertical line marks the play
 * position. */
#include <audio/audio.h>
#include <codec/codec.h>
#include <gui/app.h>
#include <gui/i18n.h>
#include <langinfo.h>
#include <math.h>
#include <pthread.h>
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
static int16_t *samples;            /* The samples are interleaved stereo at 48 kHz. */
static uint32_t frames, position;   /* Both values count frames. */
static int16_t *period;
static uint32_t quantum;
static int playing, loop;
static char file_name[128];
static char file_path[256];
static char format_text[96];
static int use_sinc;                /* The next load uses the sinc resampler when set. */
static int *column_min, *column_max, columns;

/* The functions below decode and convert audio files.  A decoded file
 * is kept as interleaved 16-bit samples in the file's channel count: the
 * upper 16 bits of the 32-bit samples libcodec returns. */

/* A load_job describes a file that the loader thread reads and decodes.
 * The main thread reads the result at the next timer tick.  job_lock
 * protects done. */
struct load_job {
    char path[256];
    int16_t *samples;
    uint32_t frames;
    char format[96];
    int error;
    int done;
    int autoplay;
    int sinc;
    uint32_t start;                 /* start is the play position in frames after the load. */
};
static struct load_job job;
static pthread_t loader;
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static int loading;

/* resample_linear converts in_frames frames at rate to out_frames frames at
 * RATE by linear interpolation between the two nearest source frames. */
static void resample_linear(const int16_t *data, int channels, uint32_t rate,
                            uint32_t in_frames, int16_t *out, uint64_t out_frames)
{
    for (uint64_t i = 0; i < out_frames; i++) {
        /* pos is the source position of output frame i in 16.16 fixed
         * point.  Its fractional part is the weight of the linear
         * interpolation. */
        uint64_t pos = i * rate * 65536 / RATE;
        uint32_t src = (uint32_t)(pos >> 16), frac = (uint32_t)(pos & 0xffff);
        uint32_t next = src + 1 < in_frames ? src + 1 : src;
        for (int c = 0; c < 2; c++) {
            uint32_t ch = channels >= 2 ? (uint32_t)c : 0;
            int32_t a = data[(size_t)src * channels + ch];
            int32_t b = data[(size_t)next * channels + ch];
            out[i * 2 + c] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
        }
    }
}

/* The sinc resampler is experimental.  Each output frame is the sum of the
 * source frames within SINC_ZEROS zero crossings of the sinc function on
 * either side, weighted by a sinc kernel with a Blackman window.  The cutoff
 * is 0.95 of the lower of the two Nyquist frequencies.  The kernel is
 * wider in source frames when the source rate is above RATE.  The coefficients are
 * computed once per file for SINC_PHASES + 1 fractional positions and
 * stored in fixed point with SINC_SHIFT fraction bits.  Each output frame
 * uses the table row of the nearest fractional position. */
#define SINC_ZEROS 16
#define SINC_PHASES 4096
#define SINC_SHIFT 15

static int resample_sinc(const int16_t *data, int file_channels, uint32_t rate,
                         uint32_t in_frames, int16_t *out, uint64_t out_frames)
{
    int channels = file_channels >= 2 ? 2 : 1;
    double cutoff = 0.95 * (rate > RATE ? (double)RATE / rate : 1.0);
    int half = (int)ceil(SINC_ZEROS / cutoff);
    int taps = 2 * half;
    /* Each channel is converted to 16-bit samples with half zero frames
     * before and after it.  The inner loop therefore needs no bounds
     * checks.  Source frame f of channel c is at in[c * stride + half + f]. */
    size_t stride = (size_t)in_frames + (size_t)taps;
    int32_t *table = malloc((size_t)(SINC_PHASES + 1) * (size_t)taps * sizeof *table);
    double *coef = malloc((size_t)taps * sizeof *coef);
    int16_t *in = calloc(stride * (size_t)channels, sizeof *in);
    if (!table || !coef || !in) {
        free(table);
        free(coef);
        free(in);
        return -1;
    }
    for (int c = 0; c < channels; c++)
        for (uint32_t f = 0; f < in_frames; f++)
            in[(size_t)c * stride + (size_t)half + f] = data[(size_t)f * file_channels + c];
    for (int p = 0; p <= SINC_PHASES; p++) {
        /* Tap j of row p weights the source frame at distance x from the
         * output position.  The coefficients of a row are scaled to a sum
         * of 1.  A constant signal therefore has the same level after the
         * conversion. */
        double frac = (double)p / SINC_PHASES, sum = 0;
        for (int j = 0; j < taps; j++) {
            double x = j - half + 1 - frac;
            double u = M_PI * cutoff * x;
            double t = M_PI * x / half;
            double window = 0.42 + 0.5 * cos(t) + 0.08 * cos(2 * t);
            coef[j] = (u == 0 ? 1.0 : sin(u) / u) * window;
            sum += coef[j];
        }
        for (int j = 0; j < taps; j++) {
            double v = coef[j] / sum * (1 << SINC_SHIFT);
            table[p * taps + j] = (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
        }
    }
    for (uint64_t i = 0; i < out_frames; i++) {
        uint64_t num = i * rate;
        uint32_t src = (uint32_t)(num / RATE);
        uint32_t p = (uint32_t)(((num % RATE) * SINC_PHASES + RATE / 2) / RATE);
        const int32_t *h = table + (size_t)p * (size_t)taps;
        for (int c = 0; c < channels; c++) {
            /* The taps cover the source frames src - half + 1 to src + half. */
            const int16_t *x = in + (size_t)c * stride + src + 1;
            int64_t acc = 0;
            for (int j = 0; j < taps; j++)
                acc += (int64_t)x[j] * h[j];
            acc = (acc + (1 << (SINC_SHIFT - 1))) >> SINC_SHIFT;
            if (acc > 32767) acc = 32767;
            if (acc < -32768) acc = -32768;
            out[i * 2 + c] = (int16_t)acc;
        }
        if (channels == 1)
            out[i * 2 + 1] = out[i * 2];
    }
    free(table);
    free(coef);
    free(in);
    return 0;
}

/* read_all decodes the whole stream into 16-bit samples; it returns the
 * number of frames, or -1. */
static long read_all(struct codec_audio *a, int channels, int16_t **result)
{
    long cap = codec_audio_frames(a) > 0 ? codec_audio_frames(a) : 65536, n = 0;
    int16_t *all = malloc((size_t)cap * channels * sizeof *all);
    int32_t *chunk = malloc(4096 * (size_t)channels * sizeof *chunk);
    while (all && chunk) {
        long got = codec_audio_read(a, chunk, 4096);
        if (got <= 0)
            break;
        if (n + got > cap) {
            int16_t *grown = realloc(all, (size_t)(cap * 2 + got) * channels * sizeof *all);
            if (!grown)
                break;
            all = grown;
            cap = cap * 2 + got;
        }
        for (long i = 0; i < got * channels; i++)
            all[n * channels + i] = (int16_t)(chunk[i] >> 16);
        n += got;
    }
    free(chunk);
    if (!all)
        return -1;
    *result = all;
    return n;
}

static int decode(const char *path, struct load_job *j)
{
    struct codec_audio *a;
    if (codec_audio_open_file(path, &a) < 0)
        return -1;
    struct codec_audio_format fmt = *codec_audio_format(a);
    int16_t *data = NULL;
    long got = read_all(a, fmt.channels, &data);
    codec_audio_close(a);
    uint32_t rate = (uint32_t)fmt.rate;
    if (got < 2 || got > 0x7fffffff) {
        free(data);
        return -1;
    }
    uint32_t in_frames = (uint32_t)got;
    uint64_t out_frames = (uint64_t)in_frames * RATE / rate;
    int16_t *out = out_frames <= 64u << 20 ? malloc((size_t)out_frames * 2 * sizeof *out) : NULL;
    if (!out) {
        free(data);
        return -1;
    }
    /* A file at RATE is copied unchanged by the linear resampler. */
    const char *method = "";
    if (rate == RATE || !j->sinc) {
        resample_linear(data, fmt.channels, rate, in_frames, out, out_frames);
        if (rate != RATE)
            method = ", linear";
    } else {
        if (resample_sinc(data, fmt.channels, rate, in_frames, out, out_frames) < 0) {
            free(data);
            free(out);
            return -1;
        }
        method = ", sinc";
    }
    free(data);
    j->samples = out;
    j->frames = (uint32_t)out_frames;
    char layout[24];
    if (fmt.channels <= 2)
        snprintf(layout, sizeof layout, "%s", fmt.channels == 2 ? "stereo" : "mono");
    else
        snprintf(layout, sizeof layout, "%d channels", fmt.channels);
    snprintf(j->format, sizeof j->format, "%u Hz %s %d-bit%s", rate, layout, fmt.bits, method);
    return 0;
}

/* load_thread reads and decodes the file and makes no calls into the user
 * interface.  The main thread continues to process server events during
 * the load. */
static void *load_thread(void *arg)
{
    struct load_job *j = arg;
    int r = decode(j->path, j);
    pthread_mutex_lock(&job_lock);
    j->error = r < 0;
    j->done = 1;
    pthread_mutex_unlock(&job_lock);
    return NULL;
}

/* load_start creates the loader thread for path.  The load starts playback
 * at frame start when autoplay is set.  It returns -1 when a load is
 * already running or when the thread cannot be created. */
static int load_start(const char *path, int autoplay, uint32_t start)
{
    if (loading)
        return -1;
    memset(&job, 0, sizeof job);
    snprintf(job.path, sizeof job.path, "%s", path);
    job.autoplay = autoplay;
    job.sinc = use_sinc;
    job.start = start;
    if (pthread_create(&loader, NULL, load_thread, &job) != 0)
        return -1;
    loading = 1;
    const char *base = strrchr(path, '/');
    char text[300];
    snprintf(text, sizeof text, _("Loading %s"), base ? base + 1 : path);
    widget_set_text(info, text);
    return 0;
}

static void show_info(void);
static void set_playing(int on);

/* load_finish installs the result of a finished load.  Only the main
 * thread modifies samples and the view. */
static void load_finish(void)
{
    pthread_mutex_lock(&job_lock);
    int done = job.done;
    pthread_mutex_unlock(&job_lock);
    if (!loading || !done)
        return;
    pthread_join(loader, NULL);
    loading = 0;
    if (job.error) {
        const char *const buttons[] = { _("Close") };
        show_info();
        app_dialog(app, _("Error"), _("The file is not a PCM WAV file."), buttons, 1);
        return;
    }
    free(samples);
    samples = job.samples;
    frames = job.frames;
    snprintf(format_text, sizeof format_text, "%s", job.format);
    snprintf(file_path, sizeof file_path, "%s", job.path);
    const char *base = strrchr(job.path, '/');
    snprintf(file_name, sizeof file_name, "%s", base ? base + 1 : job.path);
    printf("player: loaded %s, %s\n", file_name, format_text);
    fflush(stdout);
    position = job.start < frames ? job.start : 0;
    columns = 0;                    /* the overview is rebuilt on the next paint */
    if (job.autoplay)
        set_playing(1);
    show_info();
    widget_invalidate(wave);
}

/* The functions below write samples to the audio server. */

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
    widget_set_text(play_button, playing ? _("Pause") : _("Play"));
    printf("player: %s %s\n", playing ? "playing" : "paused", file_name);
    fflush(stdout);
}

/* render_period fills one period with samples from the file.  After the
 * last frame it writes silence, or it restarts at the first frame when
 * looping is enabled. */
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
                widget_set_text(play_button, _("Play"));
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
        widget_set_text(info, _("The audio server disconnected"));
        if (audio_watch) {
            app_unwatch_fd(app, audio_watch);
            audio_watch = NULL;
        }
    }
}

static void on_tick(void *arg)
{
    load_finish();
    update_time();
    widget_invalidate(wave);
}

/* The functions below draw the waveform view. */

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
        const char *text = _("No file");
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

/* The functions below implement the controls. */

static void show_info(void)
{
    char text[256];
    if (frames)
        snprintf(text, sizeof text, _("%s    %s    %u%s%u s"), file_name, format_text,
                 frames / RATE, nl_langinfo(RADIXCHAR), (frames % RATE) * 10 / RATE);
    else
        snprintf(text, sizeof text, "%s", _("No file loaded"));
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
    const char *const buttons[] = { _("Close") };
    if (app_prompt(app, _("Open"), _("File:"), name, sizeof name)) {
        set_playing(0);
        if (load_start(name, 1, 0) < 0)
            app_dialog(app, _("Error"), _("A file is still being loaded."), buttons, 1);
    }
    return 1;
}

/* on_resampling selects the resampler for later loads and loads the
 * current file again with it.  The playback of the old samples continues
 * until the new samples replace them at the same position. */
static int on_resampling(struct widget *w, void *args, void *arg)
{
    const char *const buttons[] = { _("Close") };
    int sinc = arg != NULL;
    if (sinc == use_sinc)
        return 1;
    use_sinc = sinc;
    if (file_path[0] && load_start(file_path, playing, position) < 0)
        app_dialog(app, _("Error"), _("A file is still being loaded."), buttons, 1);
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
    snprintf(text, sizeof text, _("Volume: %d%%"), w->value);
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
    if (key->code == 0x39) {        /* 0x39 is the space bar. */
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
    textdomain("player");
    struct widget *win = app_window(app, 560, 300, _("player"));
    if (!win)
        return 1;
    audio = audio_connect();
    playback = audio ? audio_playback_create(audio, "player") : NULL;
    if (!playback) {
        const char *const buttons[] = { _("Close") };
        app_dialog(app, _("Player"), _("The desktop audio service is unavailable."), buttons, 1);
        audio_disconnect(audio);
        app_destroy(app);
        return 1;
    }
    quantum = audio_playback_quantum(playback);
    period = calloc((size_t)quantum * 2, sizeof *period);

    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, _("File"));
    widget_connect(menu_add(file, _("Open..."), "open"), "clicked", on_open, NULL);
    menu_add_separator(file);
    widget_connect(menu_add(file, _("Quit"), "quit"), "clicked", on_quit, NULL);
    struct widget *resampling = menu_new(bar, _("Resampling"));
    widget_connect(menu_add(resampling, _("Linear"), NULL), "clicked", on_resampling, NULL);
    widget_connect(menu_add(resampling, _("Sinc (experimental)"), NULL), "clicked", on_resampling, &use_sinc);

    struct widget *transport = box_new(win, 0);
    widget_set_stretch(transport, 1, 0);
    play_button = button_new(transport, _("Play"));
    widget_connect(play_button, "clicked", on_play, NULL);
    widget_connect(button_new(transport, _("Stop")), "clicked", on_stop, NULL);
    loop_box = checkbox_new(transport, _("Loop"));
    widget_connect(loop_box, "toggled", on_loop, NULL);
    char volume_text[32];
    snprintf(volume_text, sizeof volume_text, _("Volume: %d%%"), 100);
    volume_label = label_new(transport, volume_text);
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

    show_info();
    int arg = 1;
    if (arg < argc && strcmp(argv[arg], "-s") == 0) {
        use_sinc = 1;
        arg++;
    }
    if (arg < argc)
        load_start(argv[arg], 1, 0);

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
