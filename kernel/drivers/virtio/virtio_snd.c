/* virtio-snd: the device, its control queue and the PCM device interface.
 * One playback stream on the TX queue and, when the device offers an
 * input stream, one capture stream on the RX queue, both 48 kHz stereo
 * S16.  The period rings live in virtio_snd_stream.c. */
#define KLOG_SUBSYS "virtio-snd"
#include <drivers/virtio/virtio_snd.h>
#include "virtio_snd_internal.h"
#include <drivers/pci.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <klog.h>
#include <errno.h>

#define VIRTIO_SND_DEVICE_MODERN 0x1059
#define SND_CTRL_MAX 96

struct virtio_snd_config {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
} __packed;

struct virtio_snd_query_info {
    struct virtio_snd_hdr hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
} __packed;

struct virtio_snd_info {
    uint32_t hda_fn_nid;
} __packed;

struct virtio_snd_pcm_info {
    struct virtio_snd_info hdr;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
} __packed;

struct snd_ctrl {
    bool done;
    uint8_t request[SND_CTRL_MAX];
    uint8_t response[SND_CTRL_MAX];
};

static int ndevices;

static void ctrl_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct snd_ctrl *x = vq->cookie[head];
    if (x)
        x->done = true;
}

int virtio_snd_ctrl_xfer(struct virtio_snd *d, const void *request, size_t request_len,
                         void *response, size_t response_len)
{
    if (request_len > SND_CTRL_MAX || response_len > SND_CTRL_MAX ||
        response_len < sizeof(struct virtio_snd_hdr))
        return -EINVAL;
    struct snd_ctrl *x = kzalloc(sizeof *x);
    if (!x)
        return -ENOMEM;
    memcpy(x->request, request, request_len);
    memset(x->response, 0xff, response_len);

    struct virtqueue *vq = d->ctrlq;
    uint16_t ids[2];
    spin_lock(&vq->lock);
    while (virtq_alloc_chain(vq, 2, ids) < 0)
        waitq_wait(&vq->waitq, &vq->lock);
    vq->desc[ids[0]].addr = virt_to_phys(x->request);
    vq->desc[ids[0]].len = (uint32_t)request_len;
    vq->desc[ids[1]].addr = virt_to_phys(x->response);
    vq->desc[ids[1]].len = (uint32_t)response_len;
    vq->desc[ids[1]].flags |= VIRTQ_DESC_F_WRITE;
    virtq_submit(vq, ids[0], x);
    while (!x->done)
        waitq_wait(&vq->waitq, &vq->lock);
    spin_unlock(&vq->lock);

    memcpy(response, x->response, response_len);
    uint32_t status = ((struct virtio_snd_hdr *)response)->code;
    kfree(x);
    if (status == VIRTIO_SND_S_OK)
        return 0;
    return status == VIRTIO_SND_S_NOT_SUPP ? -EOPNOTSUPP : -EIO;
}

int virtio_snd_pcm_command(struct virtio_snd *d, uint32_t stream_id, uint32_t code)
{
    struct virtio_snd_pcm_hdr req = {
        .hdr.code = code,
        .stream_id = stream_id,
    };
    struct virtio_snd_hdr rsp;
    return virtio_snd_ctrl_xfer(d, &req, sizeof req, &rsp, sizeof rsp);
}

/* The first stream of the direction with stereo S16 at 48 kHz. */
static int find_stream(struct virtio_snd *d, uint32_t streams, uint8_t direction,
                       uint32_t *stream_id)
{
    for (uint32_t id = 0; id < streams; id++) {
        struct virtio_snd_query_info req = {
            .hdr.code = VIRTIO_SND_R_PCM_INFO,
            .start_id = id,
            .count = 1,
            .size = sizeof(struct virtio_snd_pcm_info),
        };
        struct {
            struct virtio_snd_hdr status;
            struct virtio_snd_pcm_info info;
        } __packed rsp;
        int r = virtio_snd_ctrl_xfer(d, &req, sizeof req, &rsp, sizeof rsp);
        if (r < 0)
            continue;
        if (rsp.info.direction == direction &&
            (rsp.info.formats & (1ULL << VIRTIO_SND_PCM_FMT_S16)) &&
            (rsp.info.rates & (1ULL << VIRTIO_SND_PCM_RATE_48000)) &&
            rsp.info.channels_min <= 2 && rsp.info.channels_max >= 2) {
            *stream_id = id;
            return 0;
        }
    }
    return -ENODEV;
}

static int snd_open(struct pcm_device *pcm, struct file *f)
{
    struct virtio_snd *d = pcm->priv;
    snd_stream_reset(&d->playback);
    if (d->capture.present)
        snd_stream_reset(&d->capture);
    return 0;
}

static long snd_write(struct pcm_device *pcm, struct file *f, const char *buf, size_t n)
{
    struct virtio_snd *d = pcm->priv;
    return snd_stream_write(&d->playback, f, buf, n);
}

static long snd_read(struct pcm_device *pcm, struct file *f, char *buf, size_t n)
{
    struct virtio_snd *d = pcm->priv;
    if (!d->capture.present)
        return -ENODEV;
    return snd_stream_read(&d->capture, f, buf, n);
}

static long snd_ioctl(struct pcm_device *pcm, struct file *f, unsigned long req, uintptr_t arg)
{
    struct virtio_snd *d = pcm->priv;
    struct proc *proc = thread_current()->proc;
    struct snd_stream *capture = d->capture.present ? &d->capture : NULL;
    long r = 0;
    mutex_lock(&d->control_lock);
    switch (req) {
    case AUDIO_GET_INFO: {
        if (!vma_range_ok(proc->vm, arg, sizeof(struct audio_info), true)) {
            r = -EFAULT;
            break;
        }
        struct audio_info info = {
            .abi_version = AUDIO_ABI_VERSION,
            .capabilities = AUDIO_CAP_PLAYBACK | (capture ? AUDIO_CAP_CAPTURE : 0),
            .formats = AUDIO_FORMAT_S16_LE,
            .rates = AUDIO_RATE_48000,
            .channels_min = 2,
            .channels_max = 2,
            .period_frames_min = SND_MIN_PERIOD_FRAMES,
            .period_frames_max = SND_MAX_PERIOD_FRAMES,
            .periods_min = 2,
            .periods_max = SND_MAX_PERIODS,
        };
        memcpy((void *)arg, &info, sizeof info);
        break;
    }
    case AUDIO_SET_PARAMS:
    case AUDIO_SET_CAPTURE_PARAMS: {
        if (!vma_range_ok(proc->vm, arg, sizeof(struct audio_params), false)) {
            r = -EFAULT;
            break;
        }
        struct audio_params params;
        memcpy(&params, (void *)arg, sizeof params);
        if (req == AUDIO_SET_PARAMS)
            r = snd_stream_set_params(&d->playback, &params);
        else
            r = capture ? snd_stream_set_params(capture, &params) : -ENODEV;
        break;
    }
    case AUDIO_PREPARE:
        r = snd_stream_prepare(&d->playback);
        break;
    case AUDIO_START:
        r = snd_stream_start(&d->playback);
        break;
    case AUDIO_DROP:
    case AUDIO_DRAIN:
        r = snd_stream_stop(&d->playback);
        break;
    case AUDIO_CAPTURE_PREPARE:
        r = capture ? snd_stream_prepare(capture) : -ENODEV;
        break;
    case AUDIO_CAPTURE_START:
        r = capture ? snd_stream_start(capture) : -ENODEV;
        break;
    case AUDIO_CAPTURE_DROP:
        r = capture ? snd_stream_stop(capture) : -ENODEV;
        break;
    case AUDIO_GET_STATUS:
    case AUDIO_GET_CAPTURE_STATUS: {
        if (!vma_range_ok(proc->vm, arg, sizeof(struct audio_status), true)) {
            r = -EFAULT;
            break;
        }
        if (req == AUDIO_GET_CAPTURE_STATUS && !capture) {
            r = -ENODEV;
            break;
        }
        struct audio_status status;
        snd_stream_status(req == AUDIO_GET_STATUS ? &d->playback : capture, &status);
        memcpy((void *)arg, &status, sizeof status);
        break;
    }
    default:
        r = -ENOTTY;
        break;
    }
    mutex_unlock(&d->control_lock);
    return r;
}

static int snd_poll(struct pcm_device *pcm, struct file *f)
{
    struct virtio_snd *d = pcm->priv;
    int r = snd_stream_poll(&d->playback);
    if (d->capture.present)
        r |= snd_stream_poll(&d->capture);
    return r;
}

static void snd_close(struct pcm_device *pcm, struct file *f)
{
    struct virtio_snd *d = pcm->priv;
    mutex_lock(&d->control_lock);
    snd_stream_stop(&d->playback);
    spin_lock(&d->playback.vq->lock);
    d->playback.state = AUDIO_STATE_CLOSED;
    spin_unlock(&d->playback.vq->lock);
    if (d->capture.present) {
        snd_stream_stop(&d->capture);
        spin_lock(&d->capture.vq->lock);
        d->capture.state = AUDIO_STATE_CLOSED;
        spin_unlock(&d->capture.vq->lock);
    }
    mutex_unlock(&d->control_lock);
}

static const struct pcm_ops snd_pcm_ops = {
    .open = snd_open,
    .read = snd_read,
    .write = snd_write,
    .ioctl = snd_ioctl,
    .poll = snd_poll,
    .close = snd_close,
};

static void probe(struct pci_dev *pci)
{
    struct virtio_snd *d = kzalloc(sizeof *d);
    if (!d)
        return;
    if (virtio_pci_setup(pci, &d->vdev) < 0 ||
        virtio_negotiate(&d->vdev, 0) < 0)
        goto fail;
    d->ctrlq = virtio_queue_setup(&d->vdev, 0, ctrl_complete);
    struct virtqueue *txq = virtio_queue_setup(&d->vdev, 2, snd_tx_complete);
    struct virtqueue *rxq = virtio_queue_setup(&d->vdev, 3, snd_rx_complete);
    if (!d->ctrlq || !txq || !rxq) {
        klog_error("cannot set up the control, tx and rx queues");
        goto fail;
    }
    snd_stream_init(&d->playback, d, txq, false);
    snd_stream_init(&d->capture, d, rxq, true);
    if (virtio_start(&d->vdev) < 0)
        goto fail;
    mutex_init(&d->control_lock, "virtio_snd_control");
    volatile struct virtio_snd_config *cfg =
        (volatile struct virtio_snd_config *)d->vdev.device_cfg;
    uint32_t streams = cfg ? cfg->streams : 0;
    if (!streams || find_stream(d, streams, VIRTIO_SND_D_OUTPUT, &d->playback.stream_id) < 0) {
        klog_error("no 48 kHz stereo S16 playback stream");
        return;                 /* live IRQ refers to d; keep it allocated */
    }
    d->playback.present = true;
    d->capture.present = find_stream(d, streams, VIRTIO_SND_D_INPUT, &d->capture.stream_id) == 0;
    char name[16];
    ksnprintf(name, sizeof name, "pcm%d", ndevices);
    if (pcm_register(&d->pcm, name, &snd_pcm_ops, d) < 0)
        return;
    ndevices++;
    if (d->capture.present)
        klog_info("%s: playback stream %u, capture stream %u, vector %u",
                  name, d->playback.stream_id, d->capture.stream_id, d->vdev.vector);
    else
        klog_info("%s: playback stream %u, no capture stream, vector %u",
                  name, d->playback.stream_id, d->vdev.vector);
    return;
fail:
    klog_error("%02x:%02x.%u: initialization failed", pci->bus, pci->slot, pci->func);
    kfree(d);
}

void virtio_snd_init(void)
{
    for (size_t i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_device(i);
        if (p->vendor == VIRTIO_VENDOR && p->device == VIRTIO_SND_DEVICE_MODERN)
            probe(p);
    }
    if (!ndevices)
        klog_info("no device, audio unavailable");
}
