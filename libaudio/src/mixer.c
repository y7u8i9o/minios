/* The mixer view: every stream of the server and the master volume. */
#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static unsigned to_percent(uint32_t gain)
{
    return (unsigned)(((uint64_t)gain * 100 + 32768) >> 16);
}

static uint32_t to_gain(unsigned percent)
{
    return (uint32_t)(((uint64_t)percent << 16) / 100);
}

static struct audio_mixer_stream *find(struct audio_mixer *control, uint32_t id)
{
    for (int i = 0; i < control->count; i++)
        if (control->streams[i].id == id)
            return &control->streams[i];
    return NULL;
}

static void master_volume(void *data, struct wire_proxy *proxy, uint32_t volume)
{
    struct audio_mixer *control = data;
    control->master = to_percent(volume);
    control->generation++;
}

static void stream(void *data, struct wire_proxy *proxy, uint32_t id,
                   const char *name, uint32_t direction, uint32_t volume,
                   uint32_t state)
{
    struct audio_mixer *control = data;
    struct audio_mixer_stream *s = find(control, id);
    if (!s) {
        if (control->count == CONTROL_MAX_STREAMS)
            return;
        s = &control->streams[control->count++];
        memset(s, 0, sizeof *s);
        s->id = id;
    }
    strncpy(s->name, name ? name : "", sizeof s->name - 1);
    s->direction = (int)direction;
    s->volume = to_percent(volume);
    s->state = (int)state;
    control->generation++;
}

static void stream_removed(void *data, struct wire_proxy *proxy, uint32_t id)
{
    struct audio_mixer *control = data;
    struct audio_mixer_stream *s = find(control, id);
    if (!s)
        return;
    int index = (int)(s - control->streams);
    memmove(s, s + 1, (size_t)(control->count - index - 1) * sizeof *s);
    control->count--;
    control->generation++;
}

static void level(void *data, struct wire_proxy *proxy, uint32_t id, uint32_t peak)
{
    struct audio_mixer *control = data;
    struct audio_mixer_stream *s = find(control, id);
    if (!s)
        return;
    s->peak = peak > 32767 ? 32767 : peak;
    control->generation++;
}

static const struct audio_control_listener control_events = {
    .master_volume = master_volume,
    .stream = stream,
    .stream_removed = stream_removed,
    .level = level,
};

struct audio_mixer *audio_mixer_create(struct audio_connection *connection)
{
    if (!connection || !connection->manager) {
        errno = EINVAL;
        return NULL;
    }
    struct audio_mixer *control = calloc(1, sizeof *control);
    if (!control)
        return NULL;
    control->connection = connection;
    control->master = 100;
    control->proxy = audio_manager_get_control(connection->manager);
    if (!control->proxy ||
        audio_control_add_listener(control->proxy, &control_events, control) < 0) {
        if (control->proxy)
            wire_proxy_destroy(control->proxy);
        free(control);
        return NULL;
    }
    control->next = connection->mixers;
    connection->mixers = control;
    if (audio_mixer_sync(control) < 0) {
        audio_mixer_destroy(control);
        return NULL;
    }
    return control;
}

void audio_mixer_destroy(struct audio_mixer *control)
{
    if (!control)
        return;
    struct audio_connection *connection = control->connection;
    for (struct audio_mixer **p = &connection->mixers; *p; p = &(*p)->next)
        if (*p == control) {
            *p = control->next;
            break;
        }
    if (control->proxy) {
        audio_control_destroy(control->proxy);
        wire_display_flush(connection->display);
    }
    free(control);
}

int audio_mixer_sync(struct audio_mixer *control)
{
    if (!control) {
        errno = EINVAL;
        return -1;
    }
    return wire_display_roundtrip(control->connection->display) < 0 ? -1 : 0;
}

int audio_mixer_count(const struct audio_mixer *control)
{
    return control ? control->count : 0;
}

const struct audio_mixer_stream *audio_mixer_stream(const struct audio_mixer *control,
                                                        int index)
{
    if (!control || index < 0 || index >= control->count)
        return NULL;
    return &control->streams[index];
}

const struct audio_mixer_stream *audio_mixer_find(const struct audio_mixer *control,
                                                      uint32_t id)
{
    return control ? find((struct audio_mixer *)control, id) : NULL;
}

unsigned audio_mixer_master(const struct audio_mixer *control)
{
    return control ? control->master : 0;
}

uint32_t audio_mixer_generation(const struct audio_mixer *control)
{
    return control ? control->generation : 0;
}

int audio_mixer_set_master(struct audio_mixer *control, unsigned percent)
{
    if (!control || percent > 200) {
        errno = EINVAL;
        return -1;
    }
    audio_control_set_master_volume(control->proxy, to_gain(percent));
    return wire_display_flush(control->connection->display);
}

int audio_mixer_set_volume(struct audio_mixer *control, uint32_t id, unsigned percent)
{
    if (!control || percent > 200) {
        errno = EINVAL;
        return -1;
    }
    audio_control_set_stream_volume(control->proxy, id, to_gain(percent));
    return wire_display_flush(control->connection->display);
}
