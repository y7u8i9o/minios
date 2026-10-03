/* audiod: shared-memory playback streams mixed into the default PCM sink,
 * capture streams fed from the PCM source or from the monitor of the mix.
 * The output device clock drives everything: one period played, one
 * period mixed, one period delivered to every capture stream. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/audio.h>
#include <sys/ipc.h>
#include "audiod.h"

#define INPUT_RING 4            /* device periods stored for the capture streams */
#define LEVEL_PERIODS 5         /* periods between level events (50 ms) */

struct wire_server *server;
struct stream *streams;
uint32_t master_volume = UNITY;

static int pcm_fd = -1;
static int capture_present;
static volatile int running = 1;
static int16_t output[QUANTUM * CHANNELS];
static int32_t accumulator[QUANTUM * CHANNELS];
static int16_t input[INPUT_RING][QUANTUM * CHANNELS];
static unsigned input_head, input_count;
static uint32_t device_underruns, input_overruns, periods_mixed;

static void on_signal(int sig)
{
    running = 0;
}

static int pcm_start(void)
{
    pcm_fd = open("/dev/pcm0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (pcm_fd < 0)
        return -1;
    struct audio_info info;
    struct audio_params params = {
        AUDIO_FORMAT_S16_LE, 48000, CHANNELS, QUANTUM, DEVICE_PERIODS
    };
    if (ioctl(pcm_fd, AUDIO_GET_INFO, &info) < 0 ||
        ioctl(pcm_fd, AUDIO_SET_PARAMS, &params) < 0 ||
        ioctl(pcm_fd, AUDIO_PREPARE) < 0)
        goto fail;
    memset(output, 0, sizeof output);
    for (int i = 0; i < DEVICE_PERIODS; i++)
        if (write(pcm_fd, output, sizeof output) != sizeof output)
            goto fail;
    if (ioctl(pcm_fd, AUDIO_START) < 0)
        goto fail;
    /* The input side is optional: a host backend without a capture
     * voice never fills a period, and the capture streams then hear
     * silence from the input source. */
    if (info.capabilities & AUDIO_CAP_CAPTURE) {
        if (ioctl(pcm_fd, AUDIO_SET_CAPTURE_PARAMS, &params) == 0 &&
            ioctl(pcm_fd, AUDIO_CAPTURE_PREPARE) == 0 &&
            ioctl(pcm_fd, AUDIO_CAPTURE_START) == 0)
            capture_present = 1;
        else
            printf("audiod: capture unavailable: %s\n", strerror(errno));
    }
    return 0;

fail: {
        int error = errno;
        ioctl(pcm_fd, AUDIO_DROP);
        close(pcm_fd);
        pcm_fd = -1;
        errno = error;
        return -1;
    }
}

static void pcm_stop(void)
{
    if (capture_present)
        ioctl(pcm_fd, AUDIO_CAPTURE_DROP);
    ioctl(pcm_fd, AUDIO_DROP);
    close(pcm_fd);
    pcm_fd = -1;
}

static int16_t clamp_sample(int32_t sample)
{
    if (sample > 32767)
        return 32767;
    if (sample < -32768)
        return -32768;
    return (int16_t)sample;
}

static int32_t apply_gain(int16_t sample, uint32_t gain)
{
    return (int32_t)(((int64_t)sample * gain) >> 16);
}

/* Every captured period the device contains, newest last; the ring drops
 * its oldest period when the mixer falls behind. */
static void read_input(void)
{
    for (;;) {
        unsigned slot = (input_head + input_count) % INPUT_RING;
        if (input_count == INPUT_RING) {
            slot = input_head;
            input_head = (input_head + 1) % INPUT_RING;
            input_count--;
            input_overruns++;
        }
        ssize_t n = read(pcm_fd, input[slot], PERIOD_BYTES);
        if (n != PERIOD_BYTES)
            return;
        input_count++;
    }
}

/* A period for the capture streams of the input source, or NULL. */
static const int16_t *take_input(void)
{
    if (input_count == 0)
        return NULL;
    const int16_t *period = input[input_head];
    input_head = (input_head + 1) % INPUT_RING;
    input_count--;
    return period;
}

static void note_peak(struct stream *s, const int16_t *samples)
{
    int32_t peak = s->peak;
    for (unsigned i = 0; i < QUANTUM * CHANNELS; i++) {
        int32_t v = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];
        if (v > peak)
            peak = v;
    }
    s->peak = peak > 32767 ? 32767 : peak;
}

static void report_xrun(struct stream *s)
{
    /* A stream that has not exchanged anything since activation is
     * still pre-rolling; afterwards every missed period is an xrun and
     * the client hears about each one. */
    if (!s->started)
        return;
    s->xruns++;
    audio_stream_send_xrun(s->resource, s->xruns);
}

static void mix_playback(void)
{
    memset(accumulator, 0, sizeof accumulator);
    for (struct stream *s = streams; s; s = s->next) {
        if (s->direction != DIRECTION_PLAYBACK || !s->active)
            continue;
        int index = stream_take_buffer(s);
        if (index < 0) {
            report_xrun(s);
            continue;
        }
        s->started = 1;
        const int16_t *in = s->map + index * QUANTUM * CHANNELS;
        if (control_exists())
            note_peak(s, in);
        for (unsigned i = 0; i < QUANTUM * CHANNELS; i++)
            accumulator[i] += apply_gain(in[i], s->volume);
        s->state[index] = BUFFER_CLIENT;
        audio_stream_send_buffer_ready(s->resource, index);
        if (s->draining && s->qcount == 0)
            stream_finish_drain(s);
    }
    for (unsigned i = 0; i < QUANTUM * CHANNELS; i++)
        output[i] = clamp_sample((int32_t)(((int64_t)accumulator[i] * master_volume) >> 16));
}

/* Deliver the period to every active capture stream: the mix just
 * produced for monitor streams, the device input for the others. */
static void feed_capture(void)
{
    const int16_t *device = NULL;
    int device_taken = 0;
    for (struct stream *s = streams; s; s = s->next) {
        if (s->direction != DIRECTION_CAPTURE || !s->active)
            continue;
        if (s->source == SOURCE_INPUT && !device_taken) {
            device = take_input();
            device_taken = 1;
        }
        int index = stream_take_buffer(s);
        if (index < 0) {
            report_xrun(s);
            continue;
        }
        s->started = 1;
        int16_t *out = s->map + index * QUANTUM * CHANNELS;
        const int16_t *in = s->source == SOURCE_MONITOR ? output : device;
        if (!in)
            memset(out, 0, PERIOD_BYTES);
        else
            for (unsigned i = 0; i < QUANTUM * CHANNELS; i++)
                out[i] = clamp_sample(apply_gain(in[i], s->volume));
        if (control_exists())
            note_peak(s, out);
        s->state[index] = BUFFER_CLIENT;
        audio_stream_send_captured(s->resource, index, QUANTUM);
    }
    if (!device_taken)
        take_input();           /* nobody listens: ensure the ring remains current */
}

static int mix_period(void)
{
    mix_playback();
    feed_capture();
    if (control_exists() && ++periods_mixed % LEVEL_PERIODS == 0)
        control_send_levels();
    return write(pcm_fd, output, sizeof output) == sizeof output ? 0 : -1;
}

/* One period has been played.  Normally one period is mixed in return.
 * When the whole ring has drained the device has been playing silence,
 * so the ring is refilled with silence up to the last period and only that
 * one is mixed: the clients are never asked for more periods than time has
 * passed, so a stall does not turn into a burst of xruns. */
static int service_device(void)
{
    struct audio_status status;
    if (ioctl(pcm_fd, AUDIO_GET_STATUS, &status) < 0)
        return -1;
    if (status.state == AUDIO_STATE_RUNNING && status.queued_frames == 0) {
        device_underruns++;
        if (device_underruns == 1 || device_underruns % 100 == 0)
            printf("audiod: device underrun %u\n", device_underruns);
        memset(output, 0, sizeof output);
        for (int i = 0; i < DEVICE_PERIODS - 1; i++)
            if (write(pcm_fd, output, sizeof output) != sizeof output)
                break;
    }
    return mix_period();
}

static void flush_clients(void)
{
    for (struct wire_client *c = wire_server_first_client(server); c;) {
        struct wire_client *next = wire_client_next(c);
        if (wire_client_flush(c) < 0)
            wire_client_destroy(c);
        c = next;
    }
}

int main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (pcm_start() < 0) {
        perror("audiod: pcm0");
        return 1;
    }
    server = wire_server_create(AUDIO_SOCKET);
    if (!server) {
        perror("audiod: socket");
        pcm_stop();
        return 1;
    }
    if (!wire_global_create(server, &audio_manager_interface, 2,
                            manager_bind, NULL)) {
        perror("audiod: manager");
        wire_server_destroy(server);
        pcm_stop();
        return 1;
    }
    printf("audiod: started 48000 Hz stereo, quantum %d, capture %s\n",
           QUANTUM, capture_present ? "on" : "off");
    fflush(stdout);

    while (running) {
        struct pollfd pf[OPEN_MAX];
        struct wire_client *client[OPEN_MAX];
        int n = 0, nclients = 0;
        pf[n++] = (struct pollfd){ pcm_fd, POLLOUT | (capture_present ? POLLIN : 0), 0 };
        pf[n++] = (struct pollfd){ wire_server_fd(server), POLLIN, 0 };
        for (struct wire_client *c = wire_server_first_client(server);
             c && n < OPEN_MAX; c = wire_client_next(c)) {
            client[nclients++] = c;
            pf[n++] = (struct pollfd){ wire_client_fd(c), POLLIN, 0 };
        }
        int r = poll(pf, (unsigned)n, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        /* The hardware clock has priority over control traffic. */
        if (pf[0].revents & POLLIN)
            read_input();
        if (pf[0].revents & POLLOUT)
            if (service_device() < 0 && errno != EAGAIN)
                break;
        if (pf[0].revents & POLLERR)
            break;
        if (pf[1].revents & POLLIN)
            wire_server_accept(server);
        for (int i = 0; i < nclients; i++)
            if (pf[i + 2].revents & (POLLIN | POLLHUP))
                if (wire_client_dispatch(client[i]) < 0)
                    wire_client_destroy(client[i]);
        flush_clients();
    }
    wire_server_destroy(server);
    pcm_stop();
    printf("audiod: stopped, %u input overruns\n", input_overruns);
    return 0;
}
