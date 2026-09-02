#pragma once

#include <stdint.h>
#include <sys/types.h>

struct audio_connection;
struct audio_playback;

enum audio_playback_state {
    AUDIO_PLAYBACK_PAUSED = 0,
    AUDIO_PLAYBACK_RUNNING = 1,
    AUDIO_PLAYBACK_ERROR = 2,
};

/* Connect to audiod.  The connection owns every playback stream created
 * through it; applications may also destroy streams individually. */
struct audio_connection *audio_connect(void);
void audio_disconnect(struct audio_connection *connection);

/* Integrate the connection into an application event loop. */
int audio_connection_fd(const struct audio_connection *connection);
int audio_connection_dispatch(struct audio_connection *connection,
                              int timeout_ms);

/* Version 1 streams use interleaved, signed 16-bit little-endian stereo
 * samples at 48000 Hz.  Start the stream before a long blocking write;
 * write() returns the number of frames accepted. */
struct audio_playback *audio_playback_create(struct audio_connection *connection,
                                             const char *name);
void audio_playback_destroy(struct audio_playback *playback);
ssize_t audio_playback_write(struct audio_playback *playback,
                             const int16_t *samples, size_t frames);
int audio_playback_start(struct audio_playback *playback);
int audio_playback_pause(struct audio_playback *playback);
int audio_playback_drain(struct audio_playback *playback);
int audio_playback_set_volume(struct audio_playback *playback,
                              unsigned percent);

uint32_t audio_playback_rate(const struct audio_playback *playback);
uint32_t audio_playback_channels(const struct audio_playback *playback);
uint32_t audio_playback_quantum(const struct audio_playback *playback);
uint32_t audio_playback_xruns(const struct audio_playback *playback);
/* Buffers the client may fill right now without blocking.  An event driven
 * client refills every ready buffer whenever its connection becomes
 * readable, so the server always holds a full pool for it. */
uint32_t audio_playback_ready(const struct audio_playback *playback);
int audio_playback_state(const struct audio_playback *playback);
int audio_playback_error(const struct audio_playback *playback);
