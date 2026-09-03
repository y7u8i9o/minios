#pragma once
/* libaudio internals shared by the connection and playback code
 * (audio.c), capture streams (capture.c) and the control view
 * (control.c). */
#include <audio/audio.h>
#include <wire/client.h>
#include "core-client.h"
#include "audio-client.h"
#include <stdint.h>

#define AUDIO_SOCKET "audio"
#define AUDIO_CHANNELS 2
#define MAX_BUFFERS 16
#define DISPATCH_TIMEOUT_MS 5000
#define CONTROL_MAX_STREAMS 32

struct audio_connection {
    struct wire_display *display;
    struct wire_proxy *registry;
    struct wire_proxy *manager;
    struct audio_playback *playbacks;
    struct audio_capture *captures;
    struct audio_mixer *mixers;
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

/* Captured buffers wait in the queue with their frame counts; the head
 * one is being consumed from offset. */
struct audio_capture {
    struct audio_connection *connection;
    struct wire_proxy *proxy;
    int16_t *map;
    size_t map_size;
    uint32_t rate;
    uint32_t channels;
    uint32_t quantum;
    uint32_t buffers;
    uint8_t queue[MAX_BUFFERS];
    uint32_t frames[MAX_BUFFERS];
    unsigned qhead;
    unsigned qtail;
    unsigned qcount;
    uint32_t offset;
    uint32_t xruns;
    int configured;
    int state;
    int error;
    struct audio_capture *next;
};

struct audio_mixer {
    struct audio_connection *connection;
    struct wire_proxy *proxy;
    struct audio_mixer_stream streams[CONTROL_MAX_STREAMS];
    int count;
    unsigned master;
    uint32_t generation;
    int error;
    struct audio_mixer *next;
};

/* Dispatch until condition(arg) holds; -1 with errno on error, timeout
 * or when *error becomes set. */
int audio_wait(struct audio_connection *connection, int (*condition)(const void *arg),
               const void *arg, const int *error);
