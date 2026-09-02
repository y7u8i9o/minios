#define KLOG_SUBSYS "virtio-input"
#include <drivers/virtio/virtio_input.h>
#include <drivers/virtio/virtio.h>
#include <drivers/mouse.h>
#include <drivers/pci.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define VIRTIO_INPUT_DEVICE_MODERN 0x1052

#define VIRTIO_INPUT_CFG_ID_NAME   0x01
#define VIRTIO_INPUT_CFG_EV_BITS   0x11
#define VIRTIO_INPUT_CFG_ABS_INFO  0x12

#define INPUT_BUFFERS 32

struct virtio_input_config {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    union {
        char string[128];
        uint8_t bitmap[128];
        struct {
            uint32_t min, max, fuzz, flat, res;
        } __packed abs;
    } u;
} __packed;

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __packed;

struct input_dev {
    struct virtio_dev vdev;
    struct virtqueue *eventq;
    struct virtio_input_event *events;      /* INPUT_BUFFERS posted buffers */
    char name[64];
    bool absolute;
    uint32_t abs_max_x, abs_max_y;
    /* The report being assembled between SYN_REPORT events. Protected by
     * lock, taken under eventq->lock in the completion callback and alone
     * by virtio_input_feed. */
    struct spinlock lock;
    uint16_t ax, ay;
    int16_t dx, dy;
    int8_t dz;
    uint8_t buttons;
    bool moved, changed;
};

static struct input_dev *first;
/* Stands in when no device exists so that tests can feed events. */
static struct input_dev virtual_tablet = {
    .name = "virtual tablet", .absolute = true, .abs_max_x = MOUSE_ABS_MAX, .abs_max_y = MOUSE_ABS_MAX,
    .lock = SPINLOCK_INIT("virtio_input_virtual"),
};

static uint16_t scale_abs(uint32_t value, uint32_t max)
{
    if (max == 0 || max == MOUSE_ABS_MAX)
        return (uint16_t)MIN(value, (uint32_t)MOUSE_ABS_MAX);
    return (uint16_t)((uint64_t)MIN(value, max) * MOUSE_ABS_MAX / max);
}

/* Caller holds d->lock. */
static void handle_event(struct input_dev *d, uint16_t type, uint16_t code, uint32_t value)
{
    switch (type) {
    case EV_SYN:
        if (code != SYN_REPORT)
            return;
        if (d->moved || d->changed || d->dz) {
            struct mouse_event e = {
                .dx = d->absolute ? 0 : d->dx,
                .dy = d->absolute ? 0 : d->dy,
                .buttons = d->buttons,
                .dz = d->dz,
                .flags = d->absolute ? MOUSE_ABSOLUTE : 0,
                .ax = d->ax,
                .ay = d->ay,
            };
            mouse_push(&e);
        }
        d->dx = d->dy = 0;
        d->dz = 0;
        d->moved = d->changed = false;
        return;
    case EV_KEY: {
        uint8_t bit = code == BTN_LEFT ? 1 : code == BTN_RIGHT ? 2 : code == BTN_MIDDLE ? 4 : 0;
        if (!bit)
            return;
        uint8_t next = value ? (uint8_t)(d->buttons | bit) : (uint8_t)(d->buttons & ~bit);
        if (next != d->buttons)
            d->changed = true;
        d->buttons = next;
        return;
    }
    case EV_REL:
        if (code == REL_X) {
            d->dx = (int16_t)(d->dx + (int32_t)value);
            d->moved = true;
        } else if (code == REL_Y) {
            d->dy = (int16_t)(d->dy + (int32_t)value);
            d->moved = true;
        } else if (code == REL_WHEEL) {
            /* Linux: positive is away from the user; /dev/mouse: positive is towards the user. */
            d->dz = (int8_t)(d->dz - (int32_t)value);
        }
        return;
    case EV_ABS:
        if (code == ABS_X) {
            d->ax = scale_abs(value, d->abs_max_x);
            d->moved = true;
        } else if (code == ABS_Y) {
            d->ay = scale_abs(value, d->abs_max_y);
            d->moved = true;
        }
        return;
    }
}

void virtio_input_feed(uint16_t type, uint16_t code, uint32_t value)
{
    struct input_dev *d = first ? first : &virtual_tablet;
    spin_lock(&d->lock);
    handle_event(d, type, code, value);
    spin_unlock(&d->lock);
}

/* Post one event buffer. Caller holds eventq->lock. */
static void post(struct input_dev *d, struct virtio_input_event *ev)
{
    struct virtqueue *vq = d->eventq;
    uint16_t id;
    if (virtq_alloc_chain(vq, 1, &id) < 0)
        return;
    vq->desc[id].addr = virt_to_phys(ev);
    vq->desc[id].len = sizeof *ev;
    vq->desc[id].flags |= VIRTQ_DESC_F_WRITE;
    virtq_submit(vq, id, ev);
}

static void event_complete(struct virtqueue *vq, uint16_t head, uint32_t len)
{
    struct virtio_input_event *ev = vq->cookie[head];
    struct input_dev *d = container_of(vq->dev, struct input_dev, vdev);
    if (!ev)
        return;
    if (len >= sizeof *ev) {
        spin_lock(&d->lock);
        handle_event(d, ev->type, ev->code, ev->value);
        spin_unlock(&d->lock);
    }
    post(d, ev);
}

static uint8_t cfg_select(struct input_dev *d, uint8_t select, uint8_t subsel)
{
    volatile struct virtio_input_config *cfg = (volatile struct virtio_input_config *)d->vdev.device_cfg;
    cfg->select = select;
    cfg->subsel = subsel;
    return cfg->size;
}

static void probe(struct pci_dev *pci)
{
    struct input_dev *d = kzalloc(sizeof *d);
    if (!d)
        return;
    spinlock_init(&d->lock, "virtio_input");
    if (virtio_pci_setup(pci, &d->vdev) < 0 || virtio_negotiate(&d->vdev, 0) < 0 || !d->vdev.device_cfg)
        goto fail;
    volatile struct virtio_input_config *cfg = (volatile struct virtio_input_config *)d->vdev.device_cfg;
    uint8_t n = cfg_select(d, VIRTIO_INPUT_CFG_ID_NAME, 0);
    for (unsigned i = 0; i < n && i < sizeof d->name - 1; i++)
        d->name[i] = cfg->u.string[i];
    /* A pointer device reports ABS_X/ABS_Y or REL_X/REL_Y; keyboards are
     * left to the PS/2 driver. */
    n = cfg_select(d, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS);
    bool has_abs = n > 0 && (cfg->u.bitmap[0] & 0x03) == 0x03;
    n = cfg_select(d, VIRTIO_INPUT_CFG_EV_BITS, EV_REL);
    bool has_rel = n > 0 && (cfg->u.bitmap[0] & 0x03) == 0x03;
    if (!has_abs && !has_rel) {
        klog_info("%s: not a pointer device, ignored", d->name);
        goto fail;
    }
    d->absolute = has_abs;
    if (has_abs) {
        n = cfg_select(d, VIRTIO_INPUT_CFG_ABS_INFO, ABS_X);
        d->abs_max_x = n >= 8 ? cfg->u.abs.max : MOUSE_ABS_MAX;
        n = cfg_select(d, VIRTIO_INPUT_CFG_ABS_INFO, ABS_Y);
        d->abs_max_y = n >= 8 ? cfg->u.abs.max : MOUSE_ABS_MAX;
    }
    d->eventq = virtio_queue_setup(&d->vdev, 0, event_complete);
    if (!d->eventq) {
        klog_error("cannot set up the event queue");
        goto fail;
    }
    d->events = kzalloc(sizeof d->events[0] * INPUT_BUFFERS);
    if (!d->events)
        goto fail;
    if (virtio_start(&d->vdev) < 0)
        goto fail;
    spin_lock(&d->eventq->lock);
    for (unsigned i = 0; i < INPUT_BUFFERS && i < d->eventq->size / 2; i++)
        post(d, &d->events[i]);
    spin_unlock(&d->eventq->lock);
    if (!first)
        first = d;
    if (d->absolute)
        klog_info("%s: absolute pointer, 0..%u x 0..%u, vector %u", d->name, d->abs_max_x, d->abs_max_y,
                  d->vdev.vector);
    else
        klog_info("%s: relative pointer, vector %u", d->name, d->vdev.vector);
    return;
fail:
    kfree(d->events);
    kfree(d);
}

void virtio_input_init(void)
{
    for (size_t i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_device(i);
        if (p->vendor == VIRTIO_VENDOR && p->device == VIRTIO_INPUT_DEVICE_MODERN)
            probe(p);
    }
}
