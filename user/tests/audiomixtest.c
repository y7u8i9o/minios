/* The mixer view of audiod: stream list, per-stream and master volume,
 * levels, verified through a monitor capture of the mix. */
#include <audio/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("audiomixtest: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

static void make_period(int16_t *samples, uint32_t frames, uint32_t frequency,
                        uint32_t *phase, int16_t amplitude)
{
    for (uint32_t i = 0; i < frames; i++) {
        int16_t value = *phase < 24000 ? amplitude : -amplitude;
        samples[i * 2] = value;
        samples[i * 2 + 1] = value;
        *phase += frequency;
        if (*phase >= 48000)
            *phase -= 48000;
    }
}

static int16_t peak_of(const int16_t *samples, uint32_t frames)
{
    int16_t peak = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        int16_t v = samples[i] < 0 ? (int16_t)-samples[i] : samples[i];
        if (v > peak)
            peak = v;
    }
    return peak;
}

static struct audio_connection *connect_retry(void)
{
    for (int i = 0; i < 50; i++) {
        struct audio_connection *connection = audio_connect();
        if (connection)
            return connection;
        usleep(20000);
    }
    return NULL;
}

static const struct audio_mixer_stream *by_name(const struct audio_mixer *mixer, const char *name)
{
    for (int i = 0; i < audio_mixer_count(mixer); i++)
        if (strcmp(audio_mixer_stream(mixer, i)->name, name) == 0)
            return audio_mixer_stream(mixer, i);
    return NULL;
}

static struct audio_playback *a, *b;
static struct audio_capture *monitor;
static uint32_t quantum, phase_a, phase_b;
static int16_t *period, *captured;

/* Play n periods of both tones and return the peak of the last period
 * heard on the monitor. */
static int16_t play(int n)
{
    int16_t peak = 0;
    for (int i = 0; i < n; i++) {
        make_period(period, quantum, 440, &phase_a, 12000);
        CHECK(audio_playback_write(a, period, quantum) == (ssize_t)quantum, "write a");
        make_period(period, quantum, 660, &phase_b, 8000);
        CHECK(audio_playback_write(b, period, quantum) == (ssize_t)quantum, "write b");
        CHECK(audio_capture_read(monitor, captured, quantum) == (ssize_t)quantum, "monitor read");
        peak = peak_of(captured, quantum);
    }
    return peak;
}

int main(void)
{
    pid_t daemon = fork();
    CHECK(daemon >= 0, "fork audiod");
    if (daemon == 0) {
        char *const argv[] = { "audiod", NULL };
        execv("/bin/audiod", argv);
        _exit(127);
    }
    struct audio_connection *connection = connect_retry();
    struct audio_connection *panel = connect_retry();
    CHECK(connection && panel, "connect twice to audiod");
    if (!connection || !panel)
        goto done;
    /* The mixer view exists before any stream. */
    struct audio_mixer *mixer = audio_mixer_create(panel);
    CHECK(mixer != NULL, "create the mixer view");
    if (!mixer)
        goto done;
    CHECK(audio_mixer_count(mixer) == 0 && audio_mixer_master(mixer) == 100, "empty view at unity");

    a = audio_playback_create(connection, "mix-a");
    b = audio_playback_create(connection, "mix-b");
    monitor = audio_capture_create(connection, "mix-monitor", AUDIO_SOURCE_MONITOR);
    CHECK(a && b && monitor, "create streams");
    if (!a || !b || !monitor)
        goto done;
    quantum = audio_playback_quantum(a);
    period = malloc((size_t)quantum * 2 * sizeof *period);
    captured = malloc((size_t)quantum * 2 * sizeof *captured);

    uint32_t generation = audio_mixer_generation(mixer);
    CHECK(audio_mixer_sync(mixer) == 0, "sync after creating streams");
    CHECK(audio_mixer_generation(mixer) != generation, "the view changed");
    CHECK(audio_mixer_count(mixer) == 3, "three streams listed: %d", audio_mixer_count(mixer));
    const struct audio_mixer_stream *sa = by_name(mixer, "mix-a");
    const struct audio_mixer_stream *sb = by_name(mixer, "mix-b");
    const struct audio_mixer_stream *sm = by_name(mixer, "mix-monitor");
    CHECK(sa && sb && sm, "streams listed by name");
    if (!sa || !sb || !sm)
        goto done;
    CHECK(sa->direction == 0 && sm->direction == 1, "directions");
    CHECK(sa->volume == 100 && sb->volume == 100 && sa->state == AUDIO_PLAYBACK_PAUSED, "initial stream state");
    CHECK(audio_mixer_find(mixer, sa->id) == sa, "find by id");

    CHECK(audio_playback_start(a) == 0 && audio_playback_start(b) == 0 &&
          audio_capture_start(monitor) == 0, "start streams");
    /* The view lives on another connection: the client's requests must
     * reach the server before the view is asked about them. */
    CHECK(audio_connection_sync(connection) == 0, "client round trip");
    CHECK(audio_mixer_sync(mixer) == 0 && sa->state == AUDIO_PLAYBACK_RUNNING &&
          sm->state == AUDIO_PLAYBACK_RUNNING, "running state in the view");

    int16_t peak = play(8);
    CHECK(peak == 20000, "both tones at unity: peak %d", peak);
    /* Silence b through the mixer: only a remains. */
    CHECK(audio_mixer_set_volume(mixer, sb->id, 0) == 0, "set b to 0");
    peak = play(8);
    CHECK(peak == 12000, "b muted: peak %d", peak);
    CHECK(audio_mixer_sync(mixer) == 0 && sb->volume == 0, "b volume in the view");
    /* The master volume scales the mix. */
    CHECK(audio_mixer_set_master(mixer, 50) == 0, "set master to 50");
    peak = play(8);
    CHECK(peak == 6000, "master at 50: peak %d", peak);
    CHECK(audio_mixer_sync(mixer) == 0 && audio_mixer_master(mixer) == 50, "master in the view");
    /* A client's own volume request shows in the view too. */
    CHECK(audio_playback_set_volume(a, 25) == 0, "client sets a to 25");
    peak = play(8);
    CHECK(peak == 1500, "a at 25, master at 50: peak %d", peak);
    CHECK(audio_mixer_sync(mixer) == 0 && sa->volume == 25, "a volume in the view: %u", sa->volume);
    /* Levels: the peak of a's input before its gain. */
    CHECK(sa->peak == 12000 && sb->peak == 8000, "levels a %u b %u", sa->peak, sb->peak);
    errno = 0;
    CHECK(audio_mixer_set_master(mixer, 201) < 0 && errno == EINVAL, "reject excessive master");
    CHECK(audio_mixer_set_master(mixer, 100) == 0, "master back to unity");
    CHECK(audio_playback_drain(a) == 0 && audio_playback_drain(b) == 0, "drain");
    CHECK(audio_connection_sync(connection) == 0 && audio_mixer_sync(mixer) == 0 &&
          sa->state == AUDIO_PLAYBACK_PAUSED, "paused after drain");
    audio_playback_destroy(b);
    CHECK(audio_connection_sync(connection) == 0, "client round trip after destroy");
    CHECK(audio_mixer_sync(mixer) == 0 && audio_mixer_count(mixer) == 2 &&
          by_name(mixer, "mix-b") == NULL, "removed stream leaves the view");
    b = NULL;
    audio_mixer_destroy(mixer);
    free(period);
    free(captured);
done:
    audio_disconnect(connection);
    audio_disconnect(panel);
    kill(daemon, SIGTERM);
    int status = 0;
    waitpid(daemon, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "audiod exits cleanly");
    printf("audiomixtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
