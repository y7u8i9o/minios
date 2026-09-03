/* virtio-snd streams: the period ring of the playback stream on the TX
 * queue and of the capture stream on the RX queue.
 *
 * Playback: write() copies exactly one period into a free period and
 * submits it; the completion frees it again.  Capture: prepare submits
 * every period to the device, which fills them in order; read() copies
 * the oldest full period out and submits it again.  Both rings keep the
 * device owning its periods until their used entries arrive. */
#define KLOG_SUBSYS "virtio-snd"
#include "virtio_snd_internal.h"
#include <ipc/signal.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define RELEASE_TIMEOUT_MS 300

void snd_stream_init(struct snd_stream *s, struct virtio_snd *d, struct virtqueue *vq, bool capture)
{
    memset(s, 0, sizeof *s);
    s->dev = d;
    s->vq = vq;
    s->capture = capture;
    s->state = AUDIO_STATE_CLOSED;
}

bool snd_params_valid(const struct audio_params *p)
{
    return p->format == AUDIO_FORMAT_S16_LE && p->rate == 48000 &&
           p->channels == 2 && p->period_frames >= SND_MIN_PERIOD_FRAMES &&
           p->period_frames <= SND_MAX_PERIOD_FRAMES && p->periods >= 2 &&
           p->periods <= SND_MAX_PERIODS;
}

void snd_stream_reset(struct snd_stream *s)
{
    spin_lock(&s->vq->lock);
    s->params = (struct audio_params) {
        .format = AUDIO_FORMAT_S16_LE,
        .rate = 48000,
        .channels = 2,
        .period_frames = 480,
        .periods = 8,
    };
    s->configured = true;
    s->queued_frames = 0;
    s->transferred_frames = 0;
    s->xruns = 0;
    s->last_error = 0;
    s->draining = false;
    s->state = s->present ? AUDIO_STATE_OPEN : AUDIO_STATE_CLOSED;
    spin_unlock(&s->vq->lock);
}

int snd_stream_set_params(struct snd_stream *s, const struct audio_params *p)
{
    if (!snd_params_valid(p))
        return -EINVAL;
    int r = 0;
    spin_lock(&s->vq->lock);
    if (s->state != AUDIO_STATE_OPEN)
        r = -EBUSY;
    else {
        s->params = *p;
        s->configured = true;
    }
    spin_unlock(&s->vq->lock);
    return r;
}

static void free_ring(struct snd_stream *s)
{
    if (!s->ring)
        return;
    for (uint32_t i = 0; i < s->params.periods; i++)
        kfree(s->ring[i].data);
    kfree(s->ring);
    s->ring = NULL;
}

/* Allocate the period ring for the current parameters. */
static int setup_ring(struct snd_stream *s)
{
    free_ring(s);
    s->period_bytes = s->params.period_frames * s->params.channels * 2;
    s->generation++;
    s->ring = kzalloc(s->params.periods * sizeof *s->ring);
    if (!s->ring)
        return -ENOMEM;
    for (uint32_t i = 0; i < s->params.periods; i++) {
        struct snd_period *p = &s->ring[i];
        p->data = kzalloc(s->period_bytes);
        if (!p->data) {
            while (i)
                kfree(s->ring[--i].data);
            kfree(s->ring);
            s->ring = NULL;
            return -ENOMEM;
        }
        p->xfer.stream_id = s->stream_id;
        p->frames = s->params.period_frames;
        p->generation = s->generation;
        p->state = SND_PERIOD_FREE;
    }
    s->next = 0;
    s->inflight = 0;
    return 0;
}

static struct snd_stream *stream_of(struct virtqueue *vq)
{
    struct virtio_snd *d = container_of(vq->dev, struct virtio_snd, vdev);
    return vq == d->capture.vq ? &d->capture : &d->playback;
}

void snd_tx_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct snd_period *p = vq->cookie[head];
    if (!p)
        return;
    struct snd_stream *s = stream_of(vq);
    if (p->generation != s->generation) {
        p->state = SND_PERIOD_FREE;
        return;
    }
    if (s->queued_frames >= p->frames)
        s->queued_frames -= p->frames;
    if (p->status.status == VIRTIO_SND_S_OK)
        s->transferred_frames += p->frames;
    else {
        s->last_error = -EIO;
        s->state = AUDIO_STATE_ERROR;
    }
    p->state = SND_PERIOD_FREE;
    if (s->state == AUDIO_STATE_RUNNING && s->queued_frames == 0 && !s->draining)
        s->xruns++;
}

/* A capture period came back.  The device writes the status after the
 * data it delivered, so a short transfer (a flush at release) leaves
 * the status structure untouched; only a full period carries one. */
void snd_rx_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct snd_period *p = vq->cookie[head];
    if (!p)
        return;
    struct snd_stream *s = stream_of(vq);
    if (p->generation != s->generation) {
        p->state = SND_PERIOD_FREE;
        return;
    }
    if (s->inflight)
        s->inflight--;
    uint32_t bytes = len > sizeof p->status ? len - sizeof p->status : 0;
    if (bytes > s->period_bytes)
        bytes = s->period_bytes;
    bool full = bytes == s->period_bytes;
    if (full && p->status.status != VIRTIO_SND_S_OK) {
        s->last_error = -EIO;
        s->state = AUDIO_STATE_ERROR;
    }
    if (bytes < s->period_bytes)
        memset(p->data + bytes, 0, s->period_bytes - bytes);
    p->bytes = bytes;
    if (s->draining || s->state != AUDIO_STATE_RUNNING) {
        /* Not capturing any more: the data is dropped, the period is idle. */
        p->state = SND_PERIOD_FREE;
        return;
    }
    p->state = SND_PERIOD_FULL;
    s->queued_frames += p->frames;
    s->transferred_frames += p->frames;
    if (s->inflight == 0)
        s->xruns++;             /* the reader is late: the device has nothing to fill */
}

/* Submit period p to the RX queue.  Caller holds vq->lock. */
static int submit_rx(struct snd_stream *s, struct snd_period *p)
{
    struct virtqueue *vq = s->vq;
    uint16_t ids[3];
    if (virtq_alloc_chain(vq, 3, ids) < 0)
        return -ENOSPC;
    p->status.status = 0xffffffffu;
    p->status.latency_bytes = 0;
    p->bytes = 0;
    vq->desc[ids[0]].addr = virt_to_phys(&p->xfer);
    vq->desc[ids[0]].len = sizeof p->xfer;
    vq->desc[ids[1]].addr = virt_to_phys(p->data);
    vq->desc[ids[1]].len = s->period_bytes;
    vq->desc[ids[1]].flags |= VIRTQ_DESC_F_WRITE;
    vq->desc[ids[2]].addr = virt_to_phys(&p->status);
    vq->desc[ids[2]].len = sizeof p->status;
    vq->desc[ids[2]].flags |= VIRTQ_DESC_F_WRITE;
    p->state = SND_PERIOD_IN_FLIGHT;
    s->inflight++;
    virtq_submit(vq, ids[0], p);
    return 0;
}

int snd_stream_prepare(struct snd_stream *s)
{
    struct virtio_snd *d = s->dev;
    spin_lock(&s->vq->lock);
    bool ready = s->state == AUDIO_STATE_OPEN && s->configured;
    spin_unlock(&s->vq->lock);
    if (!ready)
        return -EINVAL;
    int r = setup_ring(s);
    if (r < 0)
        return r;
    struct virtio_snd_pcm_set_params req = {
        .hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS,
        .hdr.stream_id = s->stream_id,
        .buffer_bytes = s->period_bytes * s->params.periods,
        .period_bytes = s->period_bytes,
        .features = 0,
        .channels = (uint8_t)s->params.channels,
        .format = VIRTIO_SND_PCM_FMT_S16,
        .rate = VIRTIO_SND_PCM_RATE_48000,
    };
    struct virtio_snd_hdr rsp;
    r = virtio_snd_ctrl_xfer(d, &req, sizeof req, &rsp, sizeof rsp);
    if (r == 0)
        r = virtio_snd_pcm_command(d, s->stream_id, VIRTIO_SND_R_PCM_PREPARE);
    spin_lock(&s->vq->lock);
    s->state = r == 0 ? AUDIO_STATE_PREPARED : AUDIO_STATE_ERROR;
    s->last_error = r;
    if (r == 0 && s->capture)
        for (uint32_t i = 0; i < s->params.periods && r == 0; i++)
            r = submit_rx(s, &s->ring[i]);
    if (r < 0) {
        s->state = AUDIO_STATE_ERROR;
        s->last_error = r;
        if (s->inflight == 0)
            free_ring(s);
    }
    spin_unlock(&s->vq->lock);
    return r;
}

int snd_stream_start(struct snd_stream *s)
{
    spin_lock(&s->vq->lock);
    bool ready = s->state == AUDIO_STATE_PREPARED &&
                 (s->capture || s->queued_frames >= s->params.period_frames * 2);
    spin_unlock(&s->vq->lock);
    if (!ready)
        return -EAGAIN;
    int r = virtio_snd_pcm_command(s->dev, s->stream_id, VIRTIO_SND_R_PCM_START);
    spin_lock(&s->vq->lock);
    s->state = r == 0 ? AUDIO_STATE_RUNNING : AUDIO_STATE_ERROR;
    s->last_error = r;
    spin_unlock(&s->vq->lock);
    return r;
}

/* Stop and release the stream.  Submitted chains belong to the device
 * until their used entries arrive.  QEMU stops consuming playback chains
 * after PCM_STOP, so every submitted period is reclaimed before the
 * state changes; data not yet submitted by the caller is discarded and
 * the small, fixed hardware queue is allowed to finish.  Capture periods
 * come back when the stream is released: the wait for them is bounded,
 * and a period the device keeps stays allocated and is ignored through
 * its generation when it finally returns. */
int snd_stream_stop(struct snd_stream *s)
{
    struct virtqueue *vq = s->vq;
    spin_lock(&vq->lock);
    uint32_t state = s->state;
    if (state != AUDIO_STATE_PREPARED && state != AUDIO_STATE_RUNNING &&
        state != AUDIO_STATE_ERROR) {
        spin_unlock(&vq->lock);
        return 0;
    }
    s->draining = true;
    if (!s->capture)
        while (s->queued_frames)
            waitq_wait(&vq->waitq, &vq->lock);
    spin_unlock(&vq->lock);

    int r = 0;
    if (state == AUDIO_STATE_RUNNING)
        r = virtio_snd_pcm_command(s->dev, s->stream_id, VIRTIO_SND_R_PCM_STOP);
    int rr = virtio_snd_pcm_command(s->dev, s->stream_id, VIRTIO_SND_R_PCM_RELEASE);
    if (r == 0)
        r = rr;

    spin_lock(&vq->lock);
    if (s->capture) {
        uint64_t deadline = timer_ms() + RELEASE_TIMEOUT_MS;
        while (s->inflight && timer_ms() < deadline)
            waitq_wait_timeout(&vq->waitq, &vq->lock, deadline);
        if (s->inflight)
            klog_warn("capture: %u periods not returned by the device", s->inflight);
        s->inflight = 0;
    }
    s->draining = false;
    s->state = AUDIO_STATE_OPEN;
    s->last_error = r;
    s->queued_frames = 0;
    bool returned = true;
    for (uint32_t i = 0; s->ring && i < s->params.periods; i++)
        if (s->ring[i].state == SND_PERIOD_IN_FLIGHT)
            returned = false;
    s->generation++;            /* late completions are ignored */
    s->period_bytes = 0;
    if (returned)
        free_ring(s);
    else
        s->ring = NULL;         /* kept: the device may still write to it */
    spin_unlock(&vq->lock);
    return r;
}

void snd_stream_status(struct snd_stream *s, struct audio_status *status)
{
    spin_lock(&s->vq->lock);
    *status = (struct audio_status) {
        .state = s->state,
        .queued_frames = s->queued_frames,
        .played_frames = s->transferred_frames,
        .xruns = s->xruns,
        .last_error = s->last_error,
    };
    spin_unlock(&s->vq->lock);
}

int snd_stream_poll(struct snd_stream *s)
{
    int r = 0;
    spin_lock(&s->vq->lock);
    if (s->state == AUDIO_STATE_ERROR)
        r |= POLLERR;
    if (s->state == AUDIO_STATE_PREPARED || s->state == AUDIO_STATE_RUNNING) {
        if (s->capture) {
            if (s->ring[s->next].state == SND_PERIOD_FULL)
                r |= POLLIN;
        } else {
            for (uint32_t i = 0; i < s->params.periods; i++)
                if (s->ring[i].state == SND_PERIOD_FREE) {
                    r |= POLLOUT;
                    break;
                }
        }
    }
    spin_unlock(&s->vq->lock);
    return r;
}

long snd_stream_write(struct snd_stream *s, struct file *f, const char *buf, size_t n)
{
    if (n != s->period_bytes || n == 0)
        return -EINVAL;
    struct virtqueue *vq = s->vq;
    struct snd_period *p = NULL;
    spin_lock(&vq->lock);
    for (;;) {
        if (s->state != AUDIO_STATE_PREPARED && s->state != AUDIO_STATE_RUNNING) {
            spin_unlock(&vq->lock);
            return s->state == AUDIO_STATE_ERROR ? -EIO : -EINVAL;
        }
        for (uint32_t i = 0; i < s->params.periods; i++)
            if (s->ring[i].state == SND_PERIOD_FREE) {
                p = &s->ring[i];
                break;
            }
        if (p)
            break;
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&vq->lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&vq->lock);
            return -EINTR;
        }
        waitq_wait(&vq->waitq, &vq->lock);
    }
    p->state = SND_PERIOD_FILLING;
    spin_unlock(&vq->lock);

    memcpy(p->data, buf, n);

    spin_lock(&vq->lock);
    if (s->state != AUDIO_STATE_PREPARED && s->state != AUDIO_STATE_RUNNING) {
        p->state = SND_PERIOD_FREE;
        spin_unlock(&vq->lock);
        return -EIO;
    }
    uint16_t ids[3];
    if (virtq_alloc_chain(vq, 3, ids) < 0) {
        p->state = SND_PERIOD_FREE;
        spin_unlock(&vq->lock);
        return -EAGAIN;
    }
    p->status.status = 0xffffffffu;
    p->status.latency_bytes = 0;
    vq->desc[ids[0]].addr = virt_to_phys(&p->xfer);
    vq->desc[ids[0]].len = sizeof p->xfer;
    vq->desc[ids[1]].addr = virt_to_phys(p->data);
    vq->desc[ids[1]].len = (uint32_t)n;
    vq->desc[ids[2]].addr = virt_to_phys(&p->status);
    vq->desc[ids[2]].len = sizeof p->status;
    vq->desc[ids[2]].flags |= VIRTQ_DESC_F_WRITE;
    p->state = SND_PERIOD_IN_FLIGHT;
    s->queued_frames += p->frames;
    virtq_submit(vq, ids[0], p);
    spin_unlock(&vq->lock);
    return (long)n;
}

/* Exactly one captured period; the oldest one, in ring order. */
long snd_stream_read(struct snd_stream *s, struct file *f, char *buf, size_t n)
{
    if (n != s->period_bytes || n == 0)
        return -EINVAL;
    struct virtqueue *vq = s->vq;
    spin_lock(&vq->lock);
    for (;;) {
        if (s->state != AUDIO_STATE_PREPARED && s->state != AUDIO_STATE_RUNNING) {
            spin_unlock(&vq->lock);
            return s->state == AUDIO_STATE_ERROR ? -EIO : -EINVAL;
        }
        if (s->ring[s->next].state == SND_PERIOD_FULL)
            break;
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&vq->lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&vq->lock);
            return -EINTR;
        }
        waitq_wait(&vq->waitq, &vq->lock);
    }
    struct snd_period *p = &s->ring[s->next];
    p->state = SND_PERIOD_FILLING;      /* the reader copies it out */
    spin_unlock(&vq->lock);

    memcpy(buf, p->data, n);

    spin_lock(&vq->lock);
    if (s->queued_frames >= p->frames)
        s->queued_frames -= p->frames;
    s->next = (s->next + 1) % s->params.periods;
    int r = 0;
    if (s->state == AUDIO_STATE_PREPARED || s->state == AUDIO_STATE_RUNNING)
        r = submit_rx(s, p);
    if (r < 0)
        p->state = SND_PERIOD_FREE;
    spin_unlock(&vq->lock);
    return r < 0 ? r : (long)n;
}
