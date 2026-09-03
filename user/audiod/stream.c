/* audiod stream protocol: the manager global, playback and capture
 * streams and their buffer ownership. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/audio.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include "audiod.h"

static uint32_t next_id = 1;

struct stream *stream_find(uint32_t id)
{
    for (struct stream *s = streams; s; s = s->next)
        if (s->id == id)
            return s;
    return NULL;
}

int stream_take_buffer(struct stream *s)
{
    if (s->qcount == 0)
        return -1;
    int index = s->queue[s->qhead];
    s->qhead = (s->qhead + 1) % STREAM_BUFFERS;
    s->qcount--;
    return index;
}

static void stream_push_buffer(struct stream *s, unsigned index)
{
    s->state[index] = BUFFER_QUEUED;
    s->queue[s->qtail] = (uint8_t)index;
    s->qtail = (s->qtail + 1) % STREAM_BUFFERS;
    s->qcount++;
}

void stream_finish_drain(struct stream *s)
{
    s->active = 0;
    s->draining = 0;
    audio_stream_send_drained(s->resource);
    audio_stream_send_state(s->resource, STREAM_PAUSED, 0);
    control_notify_stream(s);
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
    control_notify_removed(s->id);
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
        if (s->direction == DIRECTION_PLAYBACK) {
            s->state[i] = BUFFER_CLIENT;
            audio_stream_send_buffer_ready(resource, i);
        } else {
            stream_push_buffer(s, i);       /* the server fills them */
        }
    }
    audio_stream_send_state(resource, STREAM_PAUSED, 0);
    control_notify_stream(s);
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
    if (s->direction == DIRECTION_PLAYBACK && frames < QUANTUM)
        memset((uint8_t *)s->map + index * PERIOD_BYTES + frames * FRAME_BYTES,
               0, (QUANTUM - frames) * FRAME_BYTES);
    stream_push_buffer(s, index);
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
    control_notify_stream(s);
}

static void handle_volume(struct wire_client *client, struct wire_resource *resource,
                          uint32_t volume)
{
    struct stream *s = resource->data;
    s->volume = volume > VOLUME_MAX ? VOLUME_MAX : volume;
    control_notify_stream(s);
}

static void handle_drain(struct wire_client *client, struct wire_resource *resource)
{
    struct stream *s = resource->data;
    if (s->direction == DIRECTION_CAPTURE) {
        /* Nothing is pending on the server side: stop at once. */
        stream_finish_drain(s);
        return;
    }
    s->draining = 1;
    if (s->qcount == 0)
        stream_finish_drain(s);
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

static void create_stream(struct wire_client *client, uint32_t id, const char *name,
                          enum stream_direction direction, enum capture_source source)
{
    struct stream *s = calloc(1, sizeof *s);
    struct wire_resource *resource = s ?
        wire_resource_create(client, &audio_stream_interface, 2, id) : NULL;
    if (!s || !resource) {
        free(s);
        return;
    }
    s->resource = resource;
    s->id = next_id++;
    s->fd = -1;
    s->volume = UNITY;
    s->direction = direction;
    s->source = source;
    strncpy(s->name, name && name[0] ? name :
            (direction == DIRECTION_PLAYBACK ? "playback" : "capture"), sizeof s->name - 1);
    s->next = streams;
    streams = s;
    wire_resource_set_listener(resource, &stream_handlers, s, stream_resource_destroy);
    printf("audiod: %s stream '%s' created\n",
           direction == DIRECTION_PLAYBACK ? "playback" :
           source == SOURCE_MONITOR ? "monitor" : "capture", s->name);
}

static void handle_create_playback(struct wire_client *client, struct wire_resource *manager,
                                   uint32_t id, const char *name)
{
    create_stream(client, id, name, DIRECTION_PLAYBACK, SOURCE_INPUT);
}

static void handle_create_capture(struct wire_client *client, struct wire_resource *manager,
                                  uint32_t id, const char *name, uint32_t source)
{
    if (source != SOURCE_INPUT && source != SOURCE_MONITOR) {
        wire_client_post_error(client, manager, 6, "unknown capture source");
        return;
    }
    create_stream(client, id, name, DIRECTION_CAPTURE, (enum capture_source)source);
}

static void handle_get_control(struct wire_client *client, struct wire_resource *manager,
                               uint32_t id)
{
    control_create(client, id);
}

static const struct audio_manager_impl manager_handlers = {
    .create_playback_stream = handle_create_playback,
    .create_capture_stream = handle_create_capture,
    .get_control = handle_get_control,
};

void manager_bind(struct wire_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wire_resource *resource =
        wire_resource_create(client, &audio_manager_interface, (int)version, id);
    if (resource)
        wire_resource_set_listener(resource, &manager_handlers, NULL, NULL);
}
