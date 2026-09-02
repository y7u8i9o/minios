#define KLOG_SUBSYS "virtio-snd"
#include <drivers/virtio/virtio_snd.h>
#include <drivers/virtio/virtio.h>
#include <drivers/pci.h>
#include <audio/pcm.h>
#include <ipc/signal.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <sync/mutex.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <klog.h>
#include <errno.h>

#define VIRTIO_SND_DEVICE_MODERN 0x1059

#define VIRTIO_SND_R_PCM_INFO       0x0100
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101
#define VIRTIO_SND_R_PCM_PREPARE    0x0102
#define VIRTIO_SND_R_PCM_RELEASE    0x0103
#define VIRTIO_SND_R_PCM_START      0x0104
#define VIRTIO_SND_R_PCM_STOP       0x0105

#define VIRTIO_SND_S_OK       0x8000
#define VIRTIO_SND_S_NOT_SUPP 0x8002

#define VIRTIO_SND_D_OUTPUT 0
#define VIRTIO_SND_PCM_FMT_S16 5
#define VIRTIO_SND_PCM_RATE_48000 7

#define SND_MAX_PERIODS 8
#define SND_CTRL_MAX 96

struct virtio_snd_config {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
} __packed;

struct virtio_snd_hdr {
    uint32_t code;
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

struct snd_ctrl {
    bool done;
    uint8_t request[SND_CTRL_MAX];
    uint8_t response[SND_CTRL_MAX];
};

enum snd_period_state {
    SND_PERIOD_FREE,
    SND_PERIOD_FILLING,
    SND_PERIOD_IN_FLIGHT,
};

struct snd_period {
    struct virtio_snd_pcm_xfer xfer;
    struct virtio_snd_pcm_status status;
    uint8_t *data;
    uint32_t frames;
    enum snd_period_state state;   /* protected by txq->lock */
};

struct virtio_snd {
    struct virtio_dev vdev;
    struct virtqueue *ctrlq;
    struct virtqueue *txq;
    struct pcm_device pcm;
    struct mutex control_lock;     /* serializes configuration sequences */
    uint32_t stream_id;
    struct audio_params params;
    struct snd_period period[SND_MAX_PERIODS];
    uint32_t period_bytes;
    uint32_t queued_frames;        /* protected by txq->lock */
    uint64_t played_frames;        /* protected by txq->lock */
    uint32_t xruns;                /* protected by txq->lock */
    int last_error;                /* protected by txq->lock */
    uint32_t state;                /* AUDIO_STATE_*, protected by txq->lock */
    bool configured;               /* protected by txq->lock */
    bool draining;                 /* protected by txq->lock */
};

static int ndevices;

static void ctrl_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct snd_ctrl *x = vq->cookie[head];
    if (x)
        x->done = true;
}

static int ctrl_xfer(struct virtio_snd *d, const void *request, size_t request_len,
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

static int pcm_command(struct virtio_snd *d, uint32_t code)
{
    struct virtio_snd_pcm_hdr req = {
        .hdr.code = code,
        .stream_id = d->stream_id,
    };
    struct virtio_snd_hdr rsp;
    return ctrl_xfer(d, &req, sizeof req, &rsp, sizeof rsp);
}

static int find_playback_stream(struct virtio_snd *d, uint32_t streams)
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
        int r = ctrl_xfer(d, &req, sizeof req, &rsp, sizeof rsp);
        if (r < 0)
            continue;
        if (rsp.info.direction == VIRTIO_SND_D_OUTPUT &&
            (rsp.info.formats & (1ULL << VIRTIO_SND_PCM_FMT_S16)) &&
            (rsp.info.rates & (1ULL << VIRTIO_SND_PCM_RATE_48000)) &&
            rsp.info.channels_min <= 2 && rsp.info.channels_max >= 2) {
            d->stream_id = id;
            return 0;
        }
    }
    return -ENODEV;
}

static void free_periods(struct virtio_snd *d)
{
    for (unsigned i = 0; i < SND_MAX_PERIODS; i++) {
        if (d->period[i].data)
            kfree(d->period[i].data);
        memset(&d->period[i], 0, sizeof d->period[i]);
    }
    d->period_bytes = 0;
}

static int alloc_periods(struct virtio_snd *d)
{
    free_periods(d);
    d->period_bytes = d->params.period_frames * d->params.channels * 2;
    for (uint32_t i = 0; i < d->params.periods; i++) {
        d->period[i].data = kmalloc(d->period_bytes);
        if (!d->period[i].data) {
            free_periods(d);
            return -ENOMEM;
        }
        memset(d->period[i].data, 0, d->period_bytes);
        d->period[i].xfer.stream_id = d->stream_id;
        d->period[i].frames = d->params.period_frames;
        d->period[i].state = SND_PERIOD_FREE;
    }
    return 0;
}

static void tx_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct snd_period *p = vq->cookie[head];
    if (!p)
        return;
    struct virtio_snd *snd = container_of(vq->dev, struct virtio_snd, vdev);
    if (snd->queued_frames >= p->frames)
        snd->queued_frames -= p->frames;
    if (p->status.status == VIRTIO_SND_S_OK)
        snd->played_frames += p->frames;
    else {
        snd->last_error = -EIO;
        snd->state = AUDIO_STATE_ERROR;
    }
    p->state = SND_PERIOD_FREE;
    if (snd->state == AUDIO_STATE_RUNNING && snd->queued_frames == 0 && !snd->draining)
        snd->xruns++;
}

static bool params_valid(const struct audio_params *p)
{
    return p->format == AUDIO_FORMAT_S16_LE && p->rate == 48000 &&
           p->channels == 2 && p->period_frames >= 120 &&
           p->period_frames <= 2048 && p->periods >= 2 &&
           p->periods <= SND_MAX_PERIODS;
}

static int snd_open(struct pcm_device *pcm, struct file *f)
{
    struct virtio_snd *d = pcm->priv;
    spin_lock(&d->txq->lock);
    d->params = (struct audio_params) {
        .format = AUDIO_FORMAT_S16_LE,
        .rate = 48000,
        .channels = 2,
        .period_frames = 480,
        .periods = 8,
    };
    d->configured = true;
    d->queued_frames = 0;
    d->played_frames = 0;
    d->xruns = 0;
    d->last_error = 0;
    d->draining = false;
    d->state = AUDIO_STATE_OPEN;
    spin_unlock(&d->txq->lock);
    return 0;
}

static long snd_write(struct pcm_device *pcm, struct file *f, const char *buf, size_t n)
{
    struct virtio_snd *d = pcm->priv;
    if (n != d->period_bytes || n == 0)
        return -EINVAL;
    struct virtqueue *vq = d->txq;
    struct snd_period *p = NULL;
    spin_lock(&vq->lock);
    for (;;) {
        if (d->state != AUDIO_STATE_PREPARED && d->state != AUDIO_STATE_RUNNING) {
            spin_unlock(&vq->lock);
            return d->state == AUDIO_STATE_ERROR ? -EIO : -EINVAL;
        }
        for (uint32_t i = 0; i < d->params.periods; i++)
            if (d->period[i].state == SND_PERIOD_FREE) {
                p = &d->period[i];
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
    if (d->state != AUDIO_STATE_PREPARED && d->state != AUDIO_STATE_RUNNING) {
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
    d->queued_frames += p->frames;
    virtq_submit(vq, ids[0], p);
    spin_unlock(&vq->lock);
    return (long)n;
}

static int snd_prepare_locked(struct virtio_snd *d)
{
    spin_lock(&d->txq->lock);
    bool ready = d->state == AUDIO_STATE_OPEN && d->configured;
    spin_unlock(&d->txq->lock);
    if (!ready)
        return -EINVAL;
    int r = alloc_periods(d);
    if (r < 0)
        return r;
    struct virtio_snd_pcm_set_params req = {
        .hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS,
        .hdr.stream_id = d->stream_id,
        .buffer_bytes = d->period_bytes * d->params.periods,
        .period_bytes = d->period_bytes,
        .features = 0,
        .channels = (uint8_t)d->params.channels,
        .format = VIRTIO_SND_PCM_FMT_S16,
        .rate = VIRTIO_SND_PCM_RATE_48000,
    };
    struct virtio_snd_hdr rsp;
    r = ctrl_xfer(d, &req, sizeof req, &rsp, sizeof rsp);
    if (r == 0)
        r = pcm_command(d, VIRTIO_SND_R_PCM_PREPARE);
    spin_lock(&d->txq->lock);
    d->state = r == 0 ? AUDIO_STATE_PREPARED : AUDIO_STATE_ERROR;
    d->last_error = r;
    spin_unlock(&d->txq->lock);
    return r;
}

static int snd_start_locked(struct virtio_snd *d)
{
    spin_lock(&d->txq->lock);
    bool ready = d->state == AUDIO_STATE_PREPARED &&
                 d->queued_frames >= d->params.period_frames * 2;
    spin_unlock(&d->txq->lock);
    if (!ready)
        return -EAGAIN;
    int r = pcm_command(d, VIRTIO_SND_R_PCM_START);
    spin_lock(&d->txq->lock);
    d->state = r == 0 ? AUDIO_STATE_RUNNING : AUDIO_STATE_ERROR;
    d->last_error = r;
    spin_unlock(&d->txq->lock);
    return r;
}

static int snd_stop_locked(struct virtio_snd *d, bool drain)
{
    spin_lock(&d->txq->lock);
    uint32_t state = d->state;
    if (state != AUDIO_STATE_PREPARED && state != AUDIO_STATE_RUNNING &&
        state != AUDIO_STATE_ERROR) {
        spin_unlock(&d->txq->lock);
        return 0;
    }
    /* Submitted virtio-snd TX chains belong to the device until their
     * used entries arrive.  QEMU stops consuming them after PCM_STOP, so
     * reclaim every submitted period before changing the device state.
     * DROP still discards all data not yet submitted by the caller; the
     * small, fixed hardware queue is allowed to finish. */
    d->draining = true;
    while (d->queued_frames)
        waitq_wait(&d->txq->waitq, &d->txq->lock);
    spin_unlock(&d->txq->lock);

    int r = 0;
    if (state == AUDIO_STATE_RUNNING)
        r = pcm_command(d, VIRTIO_SND_R_PCM_STOP);

    int rr = pcm_command(d, VIRTIO_SND_R_PCM_RELEASE);
    if (r == 0)
        r = rr;
    spin_lock(&d->txq->lock);
    d->draining = false;
    d->state = AUDIO_STATE_OPEN;
    d->last_error = r;
    spin_unlock(&d->txq->lock);
    free_periods(d);
    return r;
}

static long snd_ioctl(struct pcm_device *pcm, struct file *f, unsigned long req, uintptr_t arg)
{
    struct virtio_snd *d = pcm->priv;
    struct proc *proc = thread_current()->proc;
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
            .capabilities = AUDIO_CAP_PLAYBACK,
            .formats = AUDIO_FORMAT_S16_LE,
            .rates = AUDIO_RATE_48000,
            .channels_min = 2,
            .channels_max = 2,
            .period_frames_min = 120,
            .period_frames_max = 2048,
            .periods_min = 2,
            .periods_max = SND_MAX_PERIODS,
        };
        memcpy((void *)arg, &info, sizeof info);
        break;
    }
    case AUDIO_SET_PARAMS: {
        if (!vma_range_ok(proc->vm, arg, sizeof(struct audio_params), false)) {
            r = -EFAULT;
            break;
        }
        struct audio_params params;
        memcpy(&params, (void *)arg, sizeof params);
        if (!params_valid(&params)) {
            r = -EINVAL;
            break;
        }
        spin_lock(&d->txq->lock);
        if (d->state != AUDIO_STATE_OPEN)
            r = -EBUSY;
        else {
            d->params = params;
            d->configured = true;
        }
        spin_unlock(&d->txq->lock);
        break;
    }
    case AUDIO_PREPARE:
        r = snd_prepare_locked(d);
        break;
    case AUDIO_START:
        r = snd_start_locked(d);
        break;
    case AUDIO_DROP:
        r = snd_stop_locked(d, false);
        break;
    case AUDIO_DRAIN:
        r = snd_stop_locked(d, true);
        break;
    case AUDIO_GET_STATUS: {
        if (!vma_range_ok(proc->vm, arg, sizeof(struct audio_status), true)) {
            r = -EFAULT;
            break;
        }
        struct audio_status status;
        spin_lock(&d->txq->lock);
        status = (struct audio_status) {
            .state = d->state,
            .queued_frames = d->queued_frames,
            .played_frames = d->played_frames,
            .xruns = d->xruns,
            .last_error = d->last_error,
        };
        spin_unlock(&d->txq->lock);
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
    int r = 0;
    spin_lock(&d->txq->lock);
    if (d->state == AUDIO_STATE_ERROR)
        r |= POLLERR;
    if (d->state == AUDIO_STATE_PREPARED || d->state == AUDIO_STATE_RUNNING)
        for (uint32_t i = 0; i < d->params.periods; i++)
            if (d->period[i].state == SND_PERIOD_FREE) {
                r |= POLLOUT;
                break;
            }
    spin_unlock(&d->txq->lock);
    return r;
}

static void snd_close(struct pcm_device *pcm, struct file *f)
{
    struct virtio_snd *d = pcm->priv;
    mutex_lock(&d->control_lock);
    snd_stop_locked(d, false);
    spin_lock(&d->txq->lock);
    d->state = AUDIO_STATE_CLOSED;
    spin_unlock(&d->txq->lock);
    mutex_unlock(&d->control_lock);
}

static const struct pcm_ops snd_pcm_ops = {
    .open = snd_open,
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
    d->txq = virtio_queue_setup(&d->vdev, 2, tx_complete);
    if (!d->ctrlq || !d->txq) {
        klog_error("cannot set up control and tx queues");
        goto fail;
    }
    if (virtio_start(&d->vdev) < 0)
        goto fail;
    mutex_init(&d->control_lock, "virtio_snd_control");
    volatile struct virtio_snd_config *cfg =
        (volatile struct virtio_snd_config *)d->vdev.device_cfg;
    uint32_t streams = cfg ? cfg->streams : 0;
    if (!streams || find_playback_stream(d, streams) < 0) {
        klog_error("no 48 kHz stereo S16 playback stream");
        return;                 /* live IRQ refers to d; keep it allocated */
    }
    char name[16];
    ksnprintf(name, sizeof name, "pcm%d", ndevices);
    if (pcm_register(&d->pcm, name, &snd_pcm_ops, d) < 0)
        return;
    ndevices++;
    klog_info("%s: playback stream %u, control queue %u, tx queue %u, vector %u",
              name, d->stream_id, d->ctrlq->size, d->txq->size, d->vdev.vector);
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
        klog_warn("no playback device");
}
