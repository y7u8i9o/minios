/* audiod control interface: the mixer view of every stream, the master
 * volume and peak levels for level meters. */
#include <stdio.h>
#include <stdlib.h>
#include "audiod.h"

struct control {
    struct wire_resource *resource;
    struct control *next;
};

static struct control *controls;

int control_exists(void)
{
    return controls != NULL;
}

static void send_stream(struct wire_resource *resource, const struct stream *s)
{
    audio_control_send_stream(resource, s->id, s->name, s->direction, s->volume,
                              s->active ? STREAM_RUNNING : STREAM_PAUSED);
}

void control_notify_stream(const struct stream *s)
{
    if (!s->configured)
        return;
    for (struct control *c = controls; c; c = c->next)
        send_stream(c->resource, s);
}

void control_notify_removed(uint32_t id)
{
    for (struct control *c = controls; c; c = c->next)
        audio_control_send_stream_removed(c->resource, id);
}

void control_notify_master(void)
{
    for (struct control *c = controls; c; c = c->next)
        audio_control_send_master_volume(c->resource, master_volume);
}

/* The peak of every active stream since the last call, then reset. */
void control_send_levels(void)
{
    for (struct stream *s = streams; s; s = s->next) {
        if (!s->configured || !s->active)
            continue;
        for (struct control *c = controls; c; c = c->next)
            audio_control_send_level(c->resource, s->id, (uint32_t)s->peak);
        s->peak = 0;
    }
}

static void handle_stream_volume(struct wire_client *client, struct wire_resource *resource,
                                 uint32_t id, uint32_t volume)
{
    struct stream *s = stream_find(id);
    if (!s)
        return;
    s->volume = volume > VOLUME_MAX ? VOLUME_MAX : volume;
    control_notify_stream(s);
}

static void handle_master_volume(struct wire_client *client, struct wire_resource *resource,
                                 uint32_t volume)
{
    master_volume = volume > VOLUME_MAX ? VOLUME_MAX : volume;
    control_notify_master();
}

static void handle_destroy(struct wire_client *client, struct wire_resource *resource)
{
    wire_resource_destroy(resource);
}

static const struct audio_control_impl control_handlers = {
    .set_stream_volume = handle_stream_volume,
    .set_master_volume = handle_master_volume,
    .destroy = handle_destroy,
};

static void control_resource_destroy(struct wire_resource *resource)
{
    struct control *c = resource->data;
    for (struct control **p = &controls; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    free(c);
}

void control_create(struct wire_client *client, uint32_t id)
{
    struct control *c = calloc(1, sizeof *c);
    struct wire_resource *resource = c ?
        wire_resource_create(client, &audio_control_interface, 1, id) : NULL;
    if (!c || !resource) {
        free(c);
        return;
    }
    c->resource = resource;
    c->next = controls;
    controls = c;
    wire_resource_set_listener(resource, &control_handlers, c, control_resource_destroy);
    /* The initial view: the master volume, then every configured stream. */
    audio_control_send_master_volume(resource, master_volume);
    for (struct stream *s = streams; s; s = s->next)
        if (s->configured)
            send_stream(resource, s);
}
