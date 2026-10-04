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
/* Round trip: every request sent so far has been handled by the server. */
int audio_connection_sync(struct audio_connection *connection);

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
 * readable, so the server always contains a full pool for it. */
uint32_t audio_playback_ready(const struct audio_playback *playback);
int audio_playback_state(const struct audio_playback *playback);
int audio_playback_error(const struct audio_playback *playback);

/* ---- capture ---- */

struct audio_capture;

enum audio_capture_source {
    AUDIO_SOURCE_INPUT = 0,     /* the input device */
    AUDIO_SOURCE_MONITOR = 1,   /* the mix sent to the output */
};

/* Capture streams deliver interleaved stereo S16 at 48000 Hz.  The
 * server fills the pool from the moment the stream starts; a blocking
 * read() returns frames as they arrive.  An event driven client reads
 * audio_capture_available() frames whenever its connection is readable;
 * a buffer the client leaves unread past the pool is an xrun. */
struct audio_capture *audio_capture_create(struct audio_connection *connection,
                                           const char *name, int source);
void audio_capture_destroy(struct audio_capture *capture);
int audio_capture_start(struct audio_capture *capture);
int audio_capture_stop(struct audio_capture *capture);
ssize_t audio_capture_read(struct audio_capture *capture, int16_t *samples,
                           size_t frames);
int audio_capture_set_volume(struct audio_capture *capture, unsigned percent);
uint32_t audio_capture_rate(const struct audio_capture *capture);
uint32_t audio_capture_channels(const struct audio_capture *capture);
uint32_t audio_capture_quantum(const struct audio_capture *capture);
uint32_t audio_capture_available(const struct audio_capture *capture);
uint32_t audio_capture_xruns(const struct audio_capture *capture);
int audio_capture_state(const struct audio_capture *capture);
int audio_capture_error(const struct audio_capture *capture);

/* ---- the mixer view ---- */

struct audio_mixer;

struct audio_mixer_stream {
    uint32_t id;
    char name[48];
    int direction;              /* 0 playback, 1 capture */
    unsigned volume;            /* percent, 100 is unity */
    int state;                  /* AUDIO_PLAYBACK_PAUSED or _RUNNING */
    unsigned peak;              /* 0 to 32767, of the last 50 ms */
};

/* Every stream of every client and the master volume.  The view updates
 * as the connection dispatches; generation changes whenever it does. */
struct audio_mixer *audio_mixer_create(struct audio_connection *connection);
void audio_mixer_destroy(struct audio_mixer *control);
int audio_mixer_count(const struct audio_mixer *control);
const struct audio_mixer_stream *audio_mixer_stream(const struct audio_mixer *control,
                                                        int index);
const struct audio_mixer_stream *audio_mixer_find(const struct audio_mixer *control,
                                                      uint32_t id);
unsigned audio_mixer_master(const struct audio_mixer *control);
uint32_t audio_mixer_generation(const struct audio_mixer *control);
int audio_mixer_set_master(struct audio_mixer *control, unsigned percent);
int audio_mixer_set_volume(struct audio_mixer *control, uint32_t id, unsigned percent);
/* Round trip to the server so that the view reflects every change made
 * so far. */
int audio_mixer_sync(struct audio_mixer *control);
