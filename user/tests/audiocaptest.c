/* Capture: the raw /dev/pcm0 read side (part 1) and audiod capture
 * streams from the input and the monitor sources (part 2). */
#include <audio/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/audio.h>
#include <sys/ipc.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("audiocaptest: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

static void raw_capture(void)
{
    int fd = open("/dev/pcm0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    CHECK(fd >= 0, "open pcm0");
    if (fd < 0)
        return;
    struct audio_info info;
    CHECK(ioctl(fd, AUDIO_GET_INFO, &info) == 0, "get info");
    CHECK(info.capabilities & AUDIO_CAP_CAPTURE, "capture capability");
    struct audio_params params = { AUDIO_FORMAT_S16_LE, 48000, 2, 480, 4 };
    CHECK(ioctl(fd, AUDIO_SET_CAPTURE_PARAMS, &params) == 0, "set capture params");
    CHECK(ioctl(fd, AUDIO_CAPTURE_PREPARE) == 0, "capture prepare");
    struct pollfd pf = { fd, POLLIN, 0 };
    CHECK(poll(&pf, 1, 0) == 0, "nothing to read before start");
    CHECK(ioctl(fd, AUDIO_CAPTURE_START) == 0, "capture start");
    size_t bytes = params.period_frames * params.channels * sizeof(int16_t);
    int16_t *period = malloc(bytes);
    errno = 0;
    CHECK(read(fd, period, bytes - 4) < 0 && errno == EINVAL, "short read rejected");
    const uint32_t total = 24;
    for (uint32_t i = 0; i < total; i++) {
        pf.revents = 0;
        CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "captured period %u", i);
        CHECK(read(fd, period, bytes) == (ssize_t)bytes, "read period %u", i);
    }
    struct audio_status status;
    CHECK(ioctl(fd, AUDIO_GET_CAPTURE_STATUS, &status) == 0, "capture status");
    CHECK(status.state == AUDIO_STATE_RUNNING, "capture running %u", status.state);
    CHECK(status.played_frames >= (uint64_t)total * params.period_frames,
          "captured frames %lu", (unsigned long)status.played_frames);
    CHECK(status.last_error == 0, "capture error %d", status.last_error);
    printf("audiocaptest: raw capture %lu frames, %u overruns\n",
           (unsigned long)status.played_frames, status.xruns);
    CHECK(ioctl(fd, AUDIO_CAPTURE_DROP) == 0, "capture drop");
    CHECK(ioctl(fd, AUDIO_GET_CAPTURE_STATUS, &status) == 0, "status after drop");
    CHECK(status.state == AUDIO_STATE_OPEN && status.queued_frames == 0,
          "stopped state %u queued %u", status.state, status.queued_frames);
    /* Playback remains independent of the capture stream. */
    CHECK(ioctl(fd, AUDIO_GET_STATUS, &status) == 0 && status.state == AUDIO_STATE_OPEN,
          "playback untouched %u", status.state);
    free(period);
    close(fd);
}

static void make_period(int16_t *samples, uint32_t frames, uint32_t *phase)
{
    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = *phase < 24000 ? 12000 : -12000;
        samples[i * 2] = v;
        samples[i * 2 + 1] = v;
        *phase += 440;
        if (*phase >= 48000)
            *phase -= 48000;
    }
}

/* The largest magnitude in a period, and whether every sample is 0 or
 * exactly +-magnitude. */
static int16_t analyse(const int16_t *samples, uint32_t frames, int *exact)
{
    int16_t peak = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        int16_t v = samples[i] < 0 ? (int16_t)-samples[i] : samples[i];
        if (v > peak)
            peak = v;
    }
    *exact = 1;
    for (uint32_t i = 0; i < frames * 2; i++) {
        int16_t v = samples[i] < 0 ? (int16_t)-samples[i] : samples[i];
        if (v != 0 && v != peak)
            *exact = 0;
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

/* A tone played through audiod comes back through a monitor capture
 * stream; an input capture stream delivers the (silent) device input at
 * the same rate; a reader that stops returning buffers sees xruns. */
static void server_capture(void)
{
    pid_t daemon = fork();
    CHECK(daemon >= 0, "fork audiod");
    if (daemon == 0) {
        char *const argv[] = { "audiod", NULL };
        execv("/bin/audiod", argv);
        _exit(127);
    }
    struct audio_connection *connection = connect_retry();
    CHECK(connection != NULL, "connect to audiod");
    if (!connection)
        goto done;
    struct audio_playback *tone = audio_playback_create(connection, "capture-tone");
    struct audio_capture *monitor = audio_capture_create(connection, "monitor", AUDIO_SOURCE_MONITOR);
    struct audio_capture *input = audio_capture_create(connection, "input", AUDIO_SOURCE_INPUT);
    CHECK(tone && monitor && input, "create playback, monitor and input streams");
    errno = 0;
    CHECK(audio_capture_create(connection, "bad", 7) == NULL && errno == EINVAL, "reject unknown source");
    if (!tone || !monitor || !input)
        goto done;
    uint32_t quantum = audio_capture_quantum(monitor);
    CHECK(quantum == 480 && audio_capture_rate(monitor) == 48000 &&
          audio_capture_channels(monitor) == 2, "capture format");
    int16_t *period = malloc((size_t)quantum * 2 * sizeof *period);
    int16_t *captured = malloc((size_t)quantum * 2 * sizeof *captured);
    uint32_t phase = 0;
    for (int i = 0; i < 3; i++) {
        make_period(period, quantum, &phase);
        CHECK(audio_playback_write(tone, period, quantum) == (ssize_t)quantum, "pre-roll %d", i);
    }
    CHECK(audio_playback_start(tone) == 0 && audio_capture_start(monitor) == 0 &&
          audio_capture_start(input) == 0, "start streams");
    int tone_periods = 0, silent_inputs = 0;
    for (int i = 0; i < 30; i++) {
        CHECK(audio_capture_read(monitor, captured, quantum) == (ssize_t)quantum, "monitor read %d", i);
        int exact;
        int16_t peak = analyse(captured, quantum, &exact);
        if (peak == 12000 && exact)
            tone_periods++;
        make_period(period, quantum, &phase);
        CHECK(audio_playback_write(tone, period, quantum) == (ssize_t)quantum, "tone period %d", i);
        /* A partial read consumes part of a buffer; the rest follows. */
        CHECK(audio_capture_read(input, captured, quantum / 2) == (ssize_t)(quantum / 2), "input read %d a", i);
        CHECK(audio_capture_read(input, captured + quantum, quantum / 2) == (ssize_t)(quantum / 2), "input read %d b", i);
        peak = analyse(captured, quantum, &exact);
        if (peak == 0)
            silent_inputs++;
    }
    printf("audiocaptest: %d tone periods on the monitor, %d silent input periods\n",
           tone_periods, silent_inputs);
    CHECK(tone_periods >= 20, "the monitor hears the tone");
    CHECK(silent_inputs == 30, "the input source is silent under the none backend");
    CHECK(audio_capture_xruns(monitor) == 0 && audio_capture_xruns(input) == 0,
          "no xruns while reading promptly");
    CHECK(audio_capture_available(monitor) < quantum * 3, "available frames %u", audio_capture_available(monitor));
    /* Stop reading: the pool fills up and every further period is an xrun. */
    usleep(150000);
    CHECK(audio_connection_dispatch(connection, 100) >= 0, "dispatch after pause");
    CHECK(audio_capture_available(monitor) == quantum * 3, "full pool after a pause");
    CHECK(audio_capture_xruns(monitor) > 0, "xruns for a late reader: %u", audio_capture_xruns(monitor));
    CHECK(audio_capture_stop(monitor) == 0 && audio_capture_stop(input) == 0, "stop capture");
    CHECK(audio_playback_drain(tone) == 0, "drain tone");
    CHECK(audio_connection_dispatch(connection, 100) >= 0, "dispatch after stop");
    CHECK(audio_capture_state(monitor) == AUDIO_PLAYBACK_PAUSED, "monitor paused");
    CHECK(audio_capture_error(monitor) == 0 && audio_capture_error(input) == 0, "no capture errors");
    free(period);
    free(captured);
done:
    audio_disconnect(connection);
    kill(daemon, SIGTERM);
    int status = 0;
    waitpid(daemon, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "audiod exits cleanly");
}

int main(void)
{
    raw_capture();
    server_capture();
    printf("audiocaptest: %d failures\n", failures);
    return failures ? 1 : 0;
}
