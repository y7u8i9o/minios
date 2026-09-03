#pragma once
/* virtio-snd driver internals shared by the device file (virtio_snd.c:
 * probing, the control queue, the ioctl interface) and the stream file
 * (virtio_snd_stream.c: periods, the TX and RX queues, read and write). */
#include <drivers/virtio/virtio.h>
#include <audio/pcm.h>
#include <sync/mutex.h>
#include <minios/abi.h>

#define VIRTIO_SND_R_PCM_INFO       0x0100
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101
#define VIRTIO_SND_R_PCM_PREPARE    0x0102
#define VIRTIO_SND_R_PCM_RELEASE    0x0103
#define VIRTIO_SND_R_PCM_START      0x0104
#define VIRTIO_SND_R_PCM_STOP       0x0105

#define VIRTIO_SND_S_OK       0x8000
#define VIRTIO_SND_S_NOT_SUPP 0x8002

#define VIRTIO_SND_D_OUTPUT 0
#define VIRTIO_SND_D_INPUT  1
#define VIRTIO_SND_PCM_FMT_S16 5
#define VIRTIO_SND_PCM_RATE_48000 7

#define SND_MAX_PERIODS 8
#define SND_MAX_PERIOD_FRAMES 2048
#define SND_MIN_PERIOD_FRAMES 120
#define SND_FRAME_BYTES 4                       /* stereo S16 */

struct virtio_snd_hdr {
    uint32_t code;
} __packed;

struct virtio_snd_pcm_hdr {
    struct virtio_snd_hdr hdr;
    uint32_t stream_id;
} __packed;

struct virtio_snd_pcm_set_params {
    struct virtio_snd_pcm_hdr hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
} __packed;

struct virtio_snd_pcm_xfer {
    uint32_t stream_id;
} __packed;

struct virtio_snd_pcm_status {
    uint32_t status;
    uint32_t latency_bytes;
} __packed;

enum snd_period_state {
    SND_PERIOD_FREE,        /* owned by the driver, no data */
    SND_PERIOD_FILLING,     /* playback: a writer copies into it */
    SND_PERIOD_IN_FLIGHT,   /* submitted to the device */
    SND_PERIOD_FULL,        /* capture: holds data for the reader */
};

/* One period of the device ring. */
struct snd_period {
    struct virtio_snd_pcm_xfer xfer;
    struct virtio_snd_pcm_status status;
    uint8_t *data;
    uint32_t frames;
    uint32_t bytes;                /* capture: bytes the device returned */
    uint32_t generation;           /* the prepare that submitted it */
    enum snd_period_state state;   /* protected by vq->lock */
};

/* One direction of the device: the playback stream on the TX queue or
 * the capture stream on the RX queue.  Every field below the queue
 * pointer is protected by vq->lock; configuration sequences (prepare,
 * start, stop) are serialized by the device's control_lock. */
struct snd_stream {
    struct virtio_snd *dev;
    struct virtqueue *vq;
    uint32_t stream_id;
    bool present;                  /* the device offers this direction */
    bool capture;
    struct audio_params params;
    struct snd_period *ring;       /* params.periods entries, allocated by prepare;
                                    * freed by stop once the device returned them all,
                                    * otherwise kept: the device may still write to it */
    uint32_t period_bytes;
    uint32_t generation;
    uint32_t queued_frames;        /* playback: submitted; capture: captured, unread */
    uint64_t transferred_frames;   /* played or captured */
    uint32_t xruns;
    int last_error;
    uint32_t state;                /* AUDIO_STATE_* */
    bool configured;
    bool draining;                 /* stopping: completions are not xruns */
    uint32_t next;                 /* capture: the period the reader takes next */
    uint32_t inflight;             /* capture: periods owned by the device */
};

struct virtio_snd {
    struct virtio_dev vdev;
    struct virtqueue *ctrlq;
    struct pcm_device pcm;
    struct mutex control_lock;     /* serializes configuration sequences */
    struct snd_stream playback;
    struct snd_stream capture;
};

/* Control queue transfer; the response code decides the result. */
int virtio_snd_ctrl_xfer(struct virtio_snd *d, const void *request, size_t request_len,
                         void *response, size_t response_len);
int virtio_snd_pcm_command(struct virtio_snd *d, uint32_t stream_id, uint32_t code);

/* Stream side (virtio_snd_stream.c). */
void snd_stream_init(struct snd_stream *s, struct virtio_snd *d, struct virtqueue *vq, bool capture);
void snd_stream_reset(struct snd_stream *s);           /* on open: the default parameters */
bool snd_params_valid(const struct audio_params *p);
int snd_stream_set_params(struct snd_stream *s, const struct audio_params *p);
int snd_stream_prepare(struct snd_stream *s);
int snd_stream_start(struct snd_stream *s);
int snd_stream_stop(struct snd_stream *s);
void snd_stream_status(struct snd_stream *s, struct audio_status *status);
int snd_stream_poll(struct snd_stream *s);
long snd_stream_write(struct snd_stream *s, struct file *f, const char *buf, size_t n);
long snd_stream_read(struct snd_stream *s, struct file *f, char *buf, size_t n);
void snd_tx_complete(struct virtqueue *vq, uint16_t head, uint32_t len);
void snd_rx_complete(struct virtqueue *vq, uint16_t head, uint32_t len);
