/* Capture streams: the server fills the pool, the client reads. */
#include "internal.h"
#include <sys/audio.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void configured(void *data, struct wire_proxy *proxy, int fd,
                       uint32_t size, uint32_t format, uint32_t rate,
                       uint32_t channels, uint32_t quantum, uint32_t buffers)
{
    struct audio_capture *capture = data;
    size_t expected = (size_t)buffers * quantum * channels * sizeof(int16_t);
    if (capture->configured || format != AUDIO_FORMAT_S16_LE ||
        rate != 48000 || channels != AUDIO_CHANNELS || quantum == 0 ||
        buffers == 0 || buffers > MAX_BUFFERS || size != expected) {
        close(fd);
        capture->error = EPROTONOSUPPORT;
        capture->state = AUDIO_PLAYBACK_ERROR;
        return;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        capture->error = errno ? errno : ENOMEM;
        capture->state = AUDIO_PLAYBACK_ERROR;
        return;
    }
    capture->map = map;
    capture->map_size = size;
    capture->rate = rate;
    capture->channels = channels;
    capture->quantum = quantum;
    capture->buffers = buffers;
    capture->configured = 1;
}

static void buffer_ready(void *data, struct wire_proxy *proxy, uint32_t index)
{
}

static void captured(void *data, struct wire_proxy *proxy, uint32_t index,
                     uint32_t frames)
{
    struct audio_capture *capture = data;
    if (index >= capture->buffers || frames > capture->quantum ||
        capture->qcount >= capture->buffers) {
        capture->error = EIO;
        capture->state = AUDIO_PLAYBACK_ERROR;
        return;
    }
    capture->queue[capture->qtail] = (uint8_t)index;
    capture->frames[capture->qtail] = frames;
    capture->qtail = (capture->qtail + 1) % MAX_BUFFERS;
    capture->qcount++;
}

static void state(void *data, struct wire_proxy *proxy, uint32_t state,
                  int32_t error)
{
    struct audio_capture *capture = data;
    capture->state = (int)state;
    if (error)
        capture->error = error < 0 ? -error : error;
}

static void drained(void *data, struct wire_proxy *proxy)
{
}

static void xrun(void *data, struct wire_proxy *proxy, uint32_t count)
{
    ((struct audio_capture *)data)->xruns = count;
}

static const struct audio_stream_listener stream_events = {
    .configured = configured,
    .buffer_ready = buffer_ready,
    .state = state,
    .drained = drained,
    .xrun = xrun,
    .captured = captured,
};

static int is_configured(const void *arg)
{
    return ((const struct audio_capture *)arg)->configured;
}

static int has_data(const void *arg)
{
    return ((const struct audio_capture *)arg)->qcount != 0;
}

struct audio_capture *audio_capture_create(struct audio_connection *connection,
                                           const char *name, int source)
{
    if (!connection || !connection->manager ||
        (source != AUDIO_SOURCE_INPUT && source != AUDIO_SOURCE_MONITOR)) {
        errno = EINVAL;
        return NULL;
    }
    struct audio_capture *capture = calloc(1, sizeof *capture);
    if (!capture)
        return NULL;
    capture->connection = connection;
    capture->state = AUDIO_PLAYBACK_PAUSED;
    capture->proxy = audio_manager_create_capture_stream(
        connection->manager, name ? name : "capture", (uint32_t)source);
    if (!capture->proxy ||
        audio_stream_add_listener(capture->proxy, &stream_events, capture) < 0) {
        if (capture->proxy)
            wire_proxy_destroy(capture->proxy);
        free(capture);
        return NULL;
    }
    capture->next = connection->captures;
    connection->captures = capture;
    audio_stream_configure(capture->proxy, AUDIO_FORMAT_S16_LE, 48000,
                           AUDIO_CHANNELS);
    wire_display_flush(connection->display);
    if (audio_wait(connection, is_configured, capture, &capture->error) < 0) {
        audio_capture_destroy(capture);
        return NULL;
    }
    return capture;
}

void audio_capture_destroy(struct audio_capture *capture)
{
    if (!capture)
        return;
    struct audio_connection *connection = capture->connection;
    for (struct audio_capture **p = &connection->captures; *p; p = &(*p)->next)
        if (*p == capture) {
            *p = capture->next;
            break;
        }
    if (capture->proxy) {
        audio_stream_destroy(capture->proxy);
        wire_display_flush(connection->display);
    }
    if (capture->map)
        munmap(capture->map, capture->map_size);
    free(capture);
}

int audio_capture_start(struct audio_capture *capture)
{
    if (!capture) {
        errno = EINVAL;
        return -1;
    }
    audio_stream_set_active(capture->proxy, 1);
    return wire_display_flush(capture->connection->display);
}

int audio_capture_stop(struct audio_capture *capture)
{
    if (!capture) {
        errno = EINVAL;
        return -1;
    }
    audio_stream_set_active(capture->proxy, 0);
    return wire_display_flush(capture->connection->display);
}

/* Copy from the head buffer; hand it back once it is consumed. */
ssize_t audio_capture_read(struct audio_capture *capture, int16_t *samples,
                           size_t frames)
{
    if (!capture || (!samples && frames)) {
        errno = EINVAL;
        return -1;
    }
    size_t done = 0;
    while (done < frames) {
        if (audio_wait(capture->connection, has_data, capture, &capture->error) < 0)
            return done ? (ssize_t)done : -1;
        unsigned index = capture->queue[capture->qhead];
        uint32_t available = capture->frames[capture->qhead] - capture->offset;
        size_t count = frames - done;
        if (count > available)
            count = available;
        const int16_t *source = capture->map +
            ((size_t)index * capture->quantum + capture->offset) * capture->channels;
        memcpy(samples + done * capture->channels, source,
               count * capture->channels * sizeof(int16_t));
        done += count;
        capture->offset += (uint32_t)count;
        if (capture->offset >= capture->frames[capture->qhead]) {
            capture->offset = 0;
            capture->qhead = (capture->qhead + 1) % MAX_BUFFERS;
            capture->qcount--;
            audio_stream_queue_buffer(capture->proxy, index, 0);
            if (wire_display_flush(capture->connection->display) < 0)
                return (ssize_t)done;
        }
    }
    return (ssize_t)done;
}

int audio_capture_set_volume(struct audio_capture *capture, unsigned percent)
{
    if (!capture || percent > 200) {
        errno = EINVAL;
        return -1;
    }
    audio_stream_set_volume(capture->proxy,
                            (uint32_t)(((uint64_t)percent << 16) / 100));
    return wire_display_flush(capture->connection->display);
}

uint32_t audio_capture_rate(const struct audio_capture *capture)
{
    return capture ? capture->rate : 0;
}

uint32_t audio_capture_channels(const struct audio_capture *capture)
{
    return capture ? capture->channels : 0;
}

uint32_t audio_capture_quantum(const struct audio_capture *capture)
{
    return capture ? capture->quantum : 0;
}

uint32_t audio_capture_available(const struct audio_capture *capture)
{
    if (!capture)
        return 0;
    uint32_t frames = 0;
    for (unsigned i = 0; i < capture->qcount; i++)
        frames += capture->frames[(capture->qhead + i) % MAX_BUFFERS];
    return frames - capture->offset;
}

uint32_t audio_capture_xruns(const struct audio_capture *capture)
{
    return capture ? capture->xruns : 0;
}

int audio_capture_state(const struct audio_capture *capture)
{
    return capture ? capture->state : AUDIO_PLAYBACK_ERROR;
}

int audio_capture_error(const struct audio_capture *capture)
{
    return capture ? capture->error : EINVAL;
}
