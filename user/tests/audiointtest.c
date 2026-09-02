/* audiod/libaudio integration: two concurrent shared-memory streams. */
#include <audio/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("audiointtest: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

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

int main(void)
{
    pid_t daemon = fork();
    CHECK(daemon >= 0, "fork audiod");
    if (daemon == 0) {
        char *const argv[] = { "audiod", NULL };
        execv("/bin/audiod", argv);
        perror("audiointtest: exec audiod");
        _exit(127);
    }
    if (daemon < 0)
        return 1;

    struct audio_connection *connection = connect_retry();
    CHECK(connection != NULL, "connect to audiod");
    struct audio_playback *left = NULL;
    struct audio_playback *right = NULL;
    int16_t *a = NULL;
    int16_t *b = NULL;
    if (!connection)
        goto done;

    CHECK(audio_connection_fd(connection) >= 0, "connection descriptor");
    left = audio_playback_create(connection, "integration-440");
    right = audio_playback_create(connection, "integration-660");
    CHECK(left != NULL && right != NULL, "create two playback streams");
    if (!left || !right)
        goto done;
    CHECK(audio_playback_rate(left) == 48000 &&
          audio_playback_channels(left) == 2 &&
          audio_playback_quantum(left) == 480,
          "negotiated format");
    CHECK(audio_playback_set_volume(left, 100) == 0 &&
          audio_playback_set_volume(right, 50) == 0,
          "per-stream volume");
    errno = 0;
    CHECK(audio_playback_set_volume(right, 201) < 0 && errno == EINVAL,
          "reject excessive volume");

    uint32_t quantum = audio_playback_quantum(left);
    a = malloc((size_t)quantum * 2 * sizeof *a);
    b = malloc((size_t)quantum * 2 * sizeof *b);
    CHECK(a != NULL && b != NULL, "allocate client periods");
    if (!a || !b)
        goto done;

    uint32_t phase_a = 0, phase_b = 0;
    CHECK(audio_playback_start(left) == 0 &&
          audio_playback_start(right) == 0,
          "start two streams");
    make_period(a, quantum, 440, &phase_a, 12000);
    make_period(b, quantum, 660, &phase_b, 8000);
    CHECK(audio_playback_write(left, a, quantum) == (ssize_t)quantum &&
          audio_playback_write(right, b, quantum) == (ssize_t)quantum,
          "queue initial periods");

    const uint32_t periods = 24;
    for (uint32_t i = 1; i < periods; i++) {
        make_period(a, quantum, 440, &phase_a, 12000);
        make_period(b, quantum, 660, &phase_b, 8000);
        if (audio_playback_write(left, a, quantum) != (ssize_t)quantum ||
            audio_playback_write(right, b, quantum) != (ssize_t)quantum) {
            CHECK(0, "write mixed period %u", i);
            break;
        }
    }
    CHECK(audio_playback_drain(left) == 0, "drain first stream");
    CHECK(audio_playback_drain(right) == 0, "drain second stream");
    CHECK(audio_playback_state(left) == AUDIO_PLAYBACK_PAUSED &&
          audio_playback_state(right) == AUDIO_PLAYBACK_PAUSED,
          "streams return to paused state");
    CHECK(audio_playback_error(left) == 0 &&
          audio_playback_error(right) == 0,
          "no stream errors");
    /* Streams were started before their first write: pre-roll must not
     * count as an xrun. */
    CHECK(audio_playback_xruns(left) == 0 && audio_playback_xruns(right) == 0,
          "no xruns for blocking writers");

    /* An event driven client, as the desktop synthesizer: refill every
     * ready buffer whenever the connection becomes readable, never block. */
    CHECK(audio_playback_ready(left) == 3, "full pool after drain");
    uint32_t queued = 0;
    while (audio_playback_ready(left) > 0) {
        make_period(a, quantum, 440, &phase_a, 12000);
        CHECK(audio_playback_write(left, a, quantum) == (ssize_t)quantum,
              "pre-roll period %u", queued);
        queued++;
    }
    CHECK(queued == 3, "pre-rolled three periods");
    CHECK(audio_playback_start(left) == 0, "restart event driven stream");
    const uint32_t event_periods = 100;
    while (queued < event_periods) {
        int r = audio_connection_dispatch(connection, 1000);
        if (r < 0) {
            CHECK(0, "dispatch event driven stream");
            break;
        }
        while (queued < event_periods && audio_playback_ready(left) > 0) {
            make_period(a, quantum, 440, &phase_a, 12000);
            if (audio_playback_write(left, a, quantum) != (ssize_t)quantum) {
                CHECK(0, "event driven period %u", queued);
                break;
            }
            queued++;
        }
    }
    CHECK(audio_playback_drain(left) == 0, "drain event driven stream");
    printf("audiointtest: event driven stream played %u periods, %u xruns\n",
           queued, audio_playback_xruns(left));
    CHECK(audio_playback_xruns(left) == 0, "no xruns for event driven writer");

done:
    free(a);
    free(b);
    if (left)
        audio_playback_destroy(left);
    if (right)
        audio_playback_destroy(right);
    audio_disconnect(connection);
    kill(daemon, SIGTERM);
    int status = 0;
    waitpid(daemon, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "audiod exits cleanly");
    printf("audiointtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
