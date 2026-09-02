/* Blocking convenience API over the audiod shared-buffer protocol. */
#include <audio/audio.h>
#include <wire/client.h>
#include "core-client.h"
#include "audio-client.h"
#include <sys/audio.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUDIO_SOCKET "audio"
#define AUDIO_CHANNELS 2
#define MAX_BUFFERS 16
#define DISPATCH_TIMEOUT_MS 5000

struct audio_connection {
    struct wire_display *display;
    struct wire_proxy *registry;
    struct wire_proxy *manager;
    struct audio_playback *playbacks;
};

struct audio_playback {
    struct audio_connection *connection;
    struct wire_proxy *proxy;
    int16_t *map;
    size_t map_size;
    uint32_t rate;
    uint32_t channels;
    uint32_t quantum;
    uint32_t buffers;
    uint8_t ready[MAX_BUFFERS];
    uint8_t queue[MAX_BUFFERS];
    unsigned qhead;
    unsigned qtail;
    unsigned qcount;
    uint32_t xruns;
    int configured;
    int state;
    int error;
    int drained;
    struct audio_playback *next;
};

static void global(void *data, struct wire_proxy *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    struct audio_connection *connection = data;
    if (!connection->manager && version >= 1 &&
        strcmp(interface, "audio_manager") == 0)
        connection->manager = registry_bind(registry, name, interface, 1,
                                             &audio_manager_interface, 1);
}

static void global_remove(void *data, struct wire_proxy *registry, uint32_t name)
{
}

static const struct registry_listener registry_events = {
    .global = global,
    .global_remove = global_remove,
};

static void configured(void *data, struct wire_proxy *proxy, int fd,
                       uint32_t size, uint32_t format, uint32_t rate,
                       uint32_t channels, uint32_t quantum, uint32_t buffers)
{
    struct audio_playback *playback = data;
    size_t expected = (size_t)buffers * quantum * channels * sizeof(int16_t);
    if (playback->configured || format != AUDIO_FORMAT_S16_LE ||
        rate != 48000 || channels != AUDIO_CHANNELS || quantum == 0 ||
        buffers == 0 || buffers > MAX_BUFFERS || size != expected) {
        close(fd);
        playback->error = EPROTONOSUPPORT;
        playback->state = AUDIO_PLAYBACK_ERROR;
        return;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        playback->error = errno ? errno : ENOMEM;
        playback->state = AUDIO_PLAYBACK_ERROR;
        return;
    }
    playback->map = map;
    playback->map_size = size;
    playback->rate = rate;
    playback->channels = channels;
    playback->quantum = quantum;
    playback->buffers = buffers;
    playback->configured = 1;
}

static void buffer_ready(void *data, struct wire_proxy *proxy, uint32_t index)
{
    struct audio_playback *playback = data;
    if (index >= playback->buffers || playback->ready[index] ||
        playback->qcount >= playback->buffers) {
        playback->error = EIO;
        playback->state = AUDIO_PLAYBACK_ERROR;
        return;
    }
    playback->ready[index] = 1;
    playback->queue[playback->qtail] = (uint8_t)index;
    playback->qtail = (playback->qtail + 1) % MAX_BUFFERS;
    playback->qcount++;
}

static void state(void *data, struct wire_proxy *proxy, uint32_t state,
                  int32_t error)
{
    struct audio_playback *playback = data;
    playback->state = (int)state;
    if (error)
        playback->error = error < 0 ? -error : error;
}

static void drained(void *data, struct wire_proxy *proxy)
{
    ((struct audio_playback *)data)->drained = 1;
}

static void xrun(void *data, struct wire_proxy *proxy, uint32_t count)
{
    ((struct audio_playback *)data)->xruns = count;
}

static const struct audio_stream_listener stream_events = {
    .configured = configured,
    .buffer_ready = buffer_ready,
    .state = state,
    .drained = drained,
    .xrun = xrun,
};

int audio_connection_dispatch(struct audio_connection *connection,
                              int timeout_ms)
{
    if (!connection || !connection->display) {
        errno = EINVAL;
        return -1;
    }
    int dispatched = wire_display_dispatch_pending(connection->display);
    if (dispatched < 0)
        return -1;
    if (dispatched > 0 || timeout_ms == 0)
        return dispatched;
    if (wire_display_flush(connection->display) < 0)
        return -1;
    struct pollfd pfd = {
        .fd = wire_display_fd(connection->display),
        .events = POLLIN,
    };
    int ready = poll(&pfd, 1, timeout_ms);
    if (ready <= 0)
        return ready;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = EPIPE;
        return -1;
    }
    return wire_display_dispatch(connection->display);
}

static int wait_stream(struct audio_playback *playback,
                       int (*condition)(const struct audio_playback *))
{
    while (!condition(playback)) {
        if (playback->error) {
            errno = playback->error;
            return -1;
        }
        int dispatched = audio_connection_dispatch(playback->connection,
                                                   DISPATCH_TIMEOUT_MS);
        if (dispatched < 0)
            return -1;
        if (dispatched == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return 0;
}

static int is_configured(const struct audio_playback *playback)
{
    return playback->configured;
}

static int has_buffer(const struct audio_playback *playback)
{
    return playback->qcount != 0;
}

static int is_drained(const struct audio_playback *playback)
{
    return playback->drained;
}

struct audio_connection *audio_connect(void)
{
    signal(SIGPIPE, SIG_IGN);
    int saved_errno = 0;
    struct audio_connection *connection = calloc(1, sizeof *connection);
    if (!connection)
        return NULL;
    connection->display = wire_display_connect(AUDIO_SOCKET);
    if (!connection->display) {
        saved_errno = errno;
        goto fail;
    }
    connection->registry = display_get_registry(
        wire_display_proxy(connection->display));
    if (!connection->registry ||
        registry_add_listener(connection->registry, &registry_events,
                              connection) < 0 ||
        wire_display_roundtrip(connection->display) < 0 ||
        !connection->manager) {
        saved_errno = errno ? errno : EPROTONOSUPPORT;
        goto fail;
    }
    if (fcntl(wire_display_fd(connection->display), F_SETFL, O_NONBLOCK) < 0) {
        saved_errno = errno;
        goto fail;
    }
    return connection;

fail:
    if (connection->display)
        wire_display_disconnect(connection->display);
    free(connection);
    errno = saved_errno ? saved_errno : ECONNREFUSED;
    return NULL;
}

int audio_connection_fd(const struct audio_connection *connection)
{
    return connection && connection->display ?
        wire_display_fd(connection->display) : -1;
}

struct audio_playback *audio_playback_create(struct audio_connection *connection,
                                             const char *name)
{
    if (!connection || !connection->manager) {
        errno = EINVAL;
        return NULL;
    }
    struct audio_playback *playback = calloc(1, sizeof *playback);
    if (!playback)
        return NULL;
    playback->connection = connection;
    playback->state = AUDIO_PLAYBACK_PAUSED;
    playback->proxy = audio_manager_create_playback_stream(
        connection->manager, name ? name : "playback");
    if (!playback->proxy ||
        audio_stream_add_listener(playback->proxy, &stream_events, playback) < 0)
        goto fail;
    playback->next = connection->playbacks;
    connection->playbacks = playback;
    audio_stream_configure(playback->proxy, AUDIO_FORMAT_S16_LE, 48000,
                           AUDIO_CHANNELS);
    wire_display_flush(connection->display);
    if (wait_stream(playback, is_configured) < 0) {
        audio_playback_destroy(playback);
        return NULL;
    }
    return playback;

fail:
    if (playback->proxy)
        wire_proxy_destroy(playback->proxy);
    free(playback);
    return NULL;
}

void audio_playback_destroy(struct audio_playback *playback)
{
    if (!playback)
        return;
    struct audio_connection *connection = playback->connection;
    for (struct audio_playback **p = &connection->playbacks; *p;
         p = &(*p)->next)
        if (*p == playback) {
            *p = playback->next;
            break;
        }
    if (playback->proxy) {
        audio_stream_destroy(playback->proxy);
        wire_display_flush(connection->display);
    }
    if (playback->map)
        munmap(playback->map, playback->map_size);
    free(playback);
}

void audio_disconnect(struct audio_connection *connection)
{
    if (!connection)
        return;
    while (connection->playbacks)
        audio_playback_destroy(connection->playbacks);
    if (connection->display)
        wire_display_disconnect(connection->display);
    free(connection);
}

ssize_t audio_playback_write(struct audio_playback *playback,
                             const int16_t *samples, size_t frames)
{
    if (!playback || (!samples && frames)) {
        errno = EINVAL;
        return -1;
    }
    size_t written = 0;
    while (written < frames) {
        if (wait_stream(playback, has_buffer) < 0)
            return written ? (ssize_t)written : -1;
        unsigned index = playback->queue[playback->qhead];
        playback->qhead = (playback->qhead + 1) % MAX_BUFFERS;
        playback->qcount--;
        playback->ready[index] = 0;
        size_t count = frames - written;
        if (count > playback->quantum)
            count = playback->quantum;
        int16_t *target = playback->map +
            (size_t)index * playback->quantum * playback->channels;
        memcpy(target, samples + written * playback->channels,
               count * playback->channels * sizeof(int16_t));
        audio_stream_queue_buffer(playback->proxy, index, (uint32_t)count);
        if (wire_display_flush(playback->connection->display) < 0)
            return written ? (ssize_t)written : -1;
        written += count;
    }
    return (ssize_t)written;
}

int audio_playback_start(struct audio_playback *playback)
{
    if (!playback) {
        errno = EINVAL;
        return -1;
    }
    audio_stream_set_active(playback->proxy, 1);
    return wire_display_flush(playback->connection->display);
}

int audio_playback_pause(struct audio_playback *playback)
{
    if (!playback) {
        errno = EINVAL;
        return -1;
    }
    audio_stream_set_active(playback->proxy, 0);
    return wire_display_flush(playback->connection->display);
}

int audio_playback_drain(struct audio_playback *playback)
{
    if (!playback) {
        errno = EINVAL;
        return -1;
    }
    playback->drained = 0;
    audio_stream_set_active(playback->proxy, 1);
    audio_stream_drain(playback->proxy);
    if (wire_display_flush(playback->connection->display) < 0)
        return -1;
    return wait_stream(playback, is_drained);
}

int audio_playback_set_volume(struct audio_playback *playback,
                              unsigned percent)
{
    if (!playback || percent > 200) {
        errno = EINVAL;
        return -1;
    }
    audio_stream_set_volume(playback->proxy,
                            (uint32_t)(((uint64_t)percent << 16) / 100));
    return wire_display_flush(playback->connection->display);
}

uint32_t audio_playback_rate(const struct audio_playback *playback)
{
    return playback ? playback->rate : 0;
}

uint32_t audio_playback_channels(const struct audio_playback *playback)
{
    return playback ? playback->channels : 0;
}

uint32_t audio_playback_quantum(const struct audio_playback *playback)
{
    return playback ? playback->quantum : 0;
}

uint32_t audio_playback_xruns(const struct audio_playback *playback)
{
    return playback ? playback->xruns : 0;
}

uint32_t audio_playback_ready(const struct audio_playback *playback)
{
    return playback ? playback->qcount : 0;
}

int audio_playback_state(const struct audio_playback *playback)
{
    return playback ? playback->state : AUDIO_PLAYBACK_ERROR;
}

int audio_playback_error(const struct audio_playback *playback)
{
    return playback ? playback->error : EINVAL;
}
