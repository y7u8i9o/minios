/* audiod: shared-memory playback streams mixed into the default PCM sink. */
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
#include <sys/mman.h>
#include <wire/server.h>
#include "audio-server.h"

#define AUDIO_SOCKET "audio"
#define QUANTUM 480
#define CHANNELS 2
#define STREAM_BUFFERS 3
/* The device ring is the output latency: POLLOUT on /dev/pcm0 then means
 * "one period has been played", and the mixer produces exactly one period
 * per period played.  A client with STREAM_BUFFERS queued survives the
 * device returning several periods at once. */
#define DEVICE_PERIODS 4
#define FRAME_BYTES (CHANNELS * (int)sizeof(int16_t))
#define PERIOD_BYTES (QUANTUM * FRAME_BYTES)

#define STREAM_PAUSED 0
#define STREAM_RUNNING 1
#define STREAM_ERROR 2

enum buffer_state {
    BUFFER_CLIENT,
    BUFFER_QUEUED,
};

struct stream {
    struct wire_resource *resource;
    int fd;
    int16_t *map;
    size_t size;
    char name[48];
    enum buffer_state state[STREAM_BUFFERS];
    uint8_t queue[STREAM_BUFFERS];
    unsigned qhead, qtail, qcount;
    uint32_t volume;
    uint32_t xruns;
    int configured;
    int active;
    int started;        /* delivered a buffer since the last activation */
    int draining;
    struct stream *next;
};

static struct wire_server *server;
static struct stream *streams;
static int pcm_fd = -1;
static volatile int running = 1;
static int16_t output[QUANTUM * CHANNELS];
static int32_t accumulator[QUANTUM * CHANNELS];
static uint32_t device_underruns;

static void on_signal(int sig)
{
    running = 0;
}

static void stream_unlink(struct stream *s)
{
    for (struct stream **p = &streams; *p; p = &(*p)->next)
        if (*p == s) {
            *p = s->next;
            return;
        }
}

static void stream_resource_destroy(struct wire_resource *resource)
{
    struct stream *s = resource->data;
    stream_unlink(s);
    if (s->map)
        munmap(s->map, s->size);
    if (s->fd >= 0)
        close(s->fd);
    printf("audiod: stream '%s' removed\n", s->name);
    free(s);
}

static void stream_error(struct wire_client *client, struct wire_resource *resource,
                         uint32_t code, const char *message)
{
    audio_stream_send_state(resource, STREAM_ERROR, -(int32_t)code);
    wire_client_post_error(client, resource, code, message);
}

static void handle_configure(struct wire_client *client, struct wire_resource *resource,
                             uint32_t format, uint32_t rate, uint32_t channels)
{
    struct stream *s = resource->data;
    if (s->configured) {
        stream_error(client, resource, 1, "stream already configured");
        return;
    }
    if (format != AUDIO_FORMAT_S16_LE || rate != 48000 || channels != CHANNELS) {
        stream_error(client, resource, 2, "unsupported audio format");
        return;
    }
    s->size = STREAM_BUFFERS * PERIOD_BYTES;
    s->fd = memfd_create("audio-stream", MFD_CLOEXEC);
    if (s->fd < 0 || ftruncate(s->fd, s->size) < 0) {
        stream_error(client, resource, 3, "cannot allocate stream pool");
        return;
    }
    s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->map == MAP_FAILED) {
        s->map = NULL;
        stream_error(client, resource, 3, "cannot map stream pool");
        return;
    }
    s->configured = 1;
    audio_stream_send_configured(resource, s->fd, (uint32_t)s->size,
                                 AUDIO_FORMAT_S16_LE, 48000, CHANNELS,
                                 QUANTUM, STREAM_BUFFERS);
    for (uint32_t i = 0; i < STREAM_BUFFERS; i++) {
        s->state[i] = BUFFER_CLIENT;
        audio_stream_send_buffer_ready(resource, i);
    }
    audio_stream_send_state(resource, STREAM_PAUSED, 0);
    printf("audiod: stream '%s' configured\n", s->name);
}

static void handle_queue(struct wire_client *client, struct wire_resource *resource,
                         uint32_t index, uint32_t frames)
{
    struct stream *s = resource->data;
    if (!s->configured || index >= STREAM_BUFFERS || frames > QUANTUM ||
        s->state[index] != BUFFER_CLIENT || s->qcount == STREAM_BUFFERS) {
        stream_error(client, resource, 4, "invalid buffer submission");
        return;
    }
    if (frames < QUANTUM)
        memset((uint8_t *)s->map + index * PERIOD_BYTES + frames * FRAME_BYTES,
               0, (QUANTUM - frames) * FRAME_BYTES);
    s->state[index] = BUFFER_QUEUED;
    s->queue[s->qtail] = (uint8_t)index;
    s->qtail = (s->qtail + 1) % STREAM_BUFFERS;
    s->qcount++;
}

static void handle_active(struct wire_client *client, struct wire_resource *resource,
                          uint32_t active)
{
    struct stream *s = resource->data;
    if (!s->configured) {
        stream_error(client, resource, 5, "stream is not configured");
        return;
    }
    s->active = active != 0;
    s->started = 0;
    s->draining = 0;
    audio_stream_send_state(resource, s->active ? STREAM_RUNNING : STREAM_PAUSED, 0);
}

static void handle_volume(struct wire_client *client, struct wire_resource *resource,
                          uint32_t volume)
{
    struct stream *s = resource->data;
    s->volume = volume > 131072 ? 131072 : volume;
}

static void handle_drain(struct wire_client *client, struct wire_resource *resource)
{
    struct stream *s = resource->data;
    s->draining = 1;
    if (s->qcount == 0) {
        s->active = 0;
        s->draining = 0;
        audio_stream_send_drained(resource);
        audio_stream_send_state(resource, STREAM_PAUSED, 0);
    }
}

static void handle_destroy(struct wire_client *client, struct wire_resource *resource)
{
    wire_resource_destroy(resource);
}

static const struct audio_stream_impl stream_handlers = {
    .configure = handle_configure,
    .queue_buffer = handle_queue,
    .set_active = handle_active,
    .set_volume = handle_volume,
    .drain = handle_drain,
    .destroy = handle_destroy,
};

static void handle_create_stream(struct wire_client *client, struct wire_resource *manager,
                                 uint32_t id, const char *name)
{
    struct stream *s = calloc(1, sizeof *s);
    struct wire_resource *resource = s ?
        wire_resource_create(client, &audio_stream_interface, 1, id) : NULL;
    if (!s || !resource) {
        free(s);
        return;
    }
    s->resource = resource;
    s->fd = -1;
    s->volume = 65536;
    strncpy(s->name, name ? name : "playback", sizeof s->name - 1);
    s->next = streams;
    streams = s;
    wire_resource_set_listener(resource, &stream_handlers, s, stream_resource_destroy);
    printf("audiod: stream '%s' created\n", s->name);
}

static const struct audio_manager_impl manager_handlers = {
    .create_playback_stream = handle_create_stream,
};

static void bind_manager(struct wire_client *client, void *data,
                         uint32_t version, uint32_t id)
{
    struct wire_resource *resource =
        wire_resource_create(client, &audio_manager_interface, (int)version, id);
    if (resource)
        wire_resource_set_listener(resource, &manager_handlers, NULL, NULL);
}

static int pcm_start(void)
{
    pcm_fd = open("/dev/pcm0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (pcm_fd < 0)
        return -1;
    struct audio_params params = {
        AUDIO_FORMAT_S16_LE, 48000, CHANNELS, QUANTUM, DEVICE_PERIODS
    };
    if (ioctl(pcm_fd, AUDIO_SET_PARAMS, &params) < 0 ||
        ioctl(pcm_fd, AUDIO_PREPARE) < 0)
        goto fail;
    memset(output, 0, sizeof output);
    for (int i = 0; i < DEVICE_PERIODS; i++)
        if (write(pcm_fd, output, sizeof output) != sizeof output)
            goto fail;
    if (ioctl(pcm_fd, AUDIO_START) == 0)
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

static int16_t clamp_sample(int32_t sample)
{
    if (sample > 32767)
        return 32767;
    if (sample < -32768)
        return -32768;
    return (int16_t)sample;
}

static int mix_period(void)
{
    memset(accumulator, 0, sizeof accumulator);
    for (struct stream *s = streams; s; s = s->next) {
        if (!s->active)
            continue;
        if (s->qcount == 0) {
            /* A stream that has not delivered anything since activation
             * is still pre-rolling; afterwards every missed period is an
             * xrun and the client hears about each one. */
            if (s->started) {
                s->xruns++;
                audio_stream_send_xrun(s->resource, s->xruns);
            }
            continue;
        }
        unsigned index = s->queue[s->qhead];
        s->qhead = (s->qhead + 1) % STREAM_BUFFERS;
        s->qcount--;
        s->started = 1;
        const int16_t *input = s->map + index * QUANTUM * CHANNELS;
        for (unsigned i = 0; i < QUANTUM * CHANNELS; i++)
            accumulator[i] += (int32_t)(((int64_t)input[i] * s->volume) >> 16);
        s->state[index] = BUFFER_CLIENT;
        audio_stream_send_buffer_ready(s->resource, index);
        if (s->draining && s->qcount == 0) {
            s->active = 0;
            s->draining = 0;
            audio_stream_send_drained(s->resource);
            audio_stream_send_state(s->resource, STREAM_PAUSED, 0);
        }
    }
    for (unsigned i = 0; i < QUANTUM * CHANNELS; i++)
        output[i] = clamp_sample(accumulator[i]);
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
        ioctl(pcm_fd, AUDIO_DROP);
        close(pcm_fd);
        return 1;
    }
    if (!wire_global_create(server, &audio_manager_interface, 1,
                            bind_manager, NULL)) {
        perror("audiod: manager");
        wire_server_destroy(server);
        ioctl(pcm_fd, AUDIO_DROP);
        close(pcm_fd);
        return 1;
    }
    printf("audiod: started 48000 Hz stereo, quantum %d\n", QUANTUM);
    fflush(stdout);

    while (running) {
        struct pollfd pf[OPEN_MAX];
        struct wire_client *client[OPEN_MAX];
        int n = 0, nclients = 0;
        pf[n++] = (struct pollfd){ pcm_fd, POLLOUT, 0 };
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
    ioctl(pcm_fd, AUDIO_DROP);
    close(pcm_fd);
    printf("audiod: stopped\n");
    return 0;
}
