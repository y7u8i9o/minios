#pragma once
/* audiod internals: the mixer (main.c), the stream protocol (stream.c)
 * and the control interface (control.c). */
#include <stdint.h>
#include <stddef.h>
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
#define UNITY 65536u
#define VOLUME_MAX 131072u

#define STREAM_PAUSED 0
#define STREAM_RUNNING 1
#define STREAM_ERROR 2

enum stream_direction { DIRECTION_PLAYBACK = 0, DIRECTION_CAPTURE = 1 };
enum capture_source { SOURCE_INPUT = 0, SOURCE_MONITOR = 1 };

/* Playback: CLIENT buffers are being filled by the client, QUEUED ones
 * wait in the queue for the mixer.  Capture: QUEUED buffers wait in the
 * queue for the server to fill, CLIENT ones hold captured data. */
enum buffer_state {
    BUFFER_CLIENT,
    BUFFER_QUEUED,
};

struct stream {
    struct wire_resource *resource;
    uint32_t id;                    /* server wide, for the control interface */
    int fd;
    int16_t *map;
    size_t size;
    char name[48];
    enum stream_direction direction;
    enum capture_source source;
    enum buffer_state state[STREAM_BUFFERS];
    uint8_t queue[STREAM_BUFFERS];
    unsigned qhead, qtail, qcount;
    uint32_t volume;                /* Q16.16 */
    uint32_t xruns;
    int configured;
    int active;
    int started;        /* delivered a buffer since the last activation */
    int draining;
    int32_t peak;       /* largest sample magnitude since the last level event */
    struct stream *next;
};

/* main.c */
extern struct wire_server *server;
extern struct stream *streams;
extern uint32_t master_volume;

/* stream.c */
void manager_bind(struct wire_client *client, void *data, uint32_t version, uint32_t id);
struct stream *stream_find(uint32_t id);
/* Take the buffer at the head of the queue; -1 when there is none. */
int stream_take_buffer(struct stream *s);
void stream_finish_drain(struct stream *s);

/* control.c */
void control_create(struct wire_client *client, uint32_t id);
int control_exists(void);
void control_notify_stream(const struct stream *s);
void control_notify_removed(uint32_t id);
void control_notify_master(void);
void control_send_levels(void);
