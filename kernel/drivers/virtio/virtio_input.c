/* virtio-input: keyboards, mice and tablets. The device reports evdev
 * events, which are the events of the input core, so they are reported
 * unchanged; the capabilities and axis ranges come from the
 * configuration space. */
#define KLOG_SUBSYS "virtio-input"
#include <drivers/virtio/virtio_input.h>
#include <drivers/virtio/virtio.h>
#include <drivers/pci.h>
#include <input/input.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define VIRTIO_INPUT_DEVICE_MODERN 0x1052

#define VIRTIO_INPUT_CFG_ID_NAME   0x01
#define VIRTIO_INPUT_CFG_ID_DEVIDS 0x03
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
        struct {
            uint16_t bustype, vendor, product, version;
        } __packed ids;
    } u;
} __packed;

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __packed;

struct vinput {
    struct virtio_dev vdev;
    struct virtqueue *eventq;
    struct virtio_input_event *events;      /* INPUT_BUFFERS posted buffers */
    struct input_dev dev;
};

static struct vinput *first;

#if CONFIG_TESTS
/* A tablet that tests feed when no device is attached, and the target of
 * the absolute cursor positioning of the GUI tests. */
static struct input_dev virtual_tablet;
#endif

void virtio_input_feed(uint16_t type, uint16_t code, uint32_t value)
{
#if CONFIG_TESTS
    struct input_dev *d = first ? &first->dev : &virtual_tablet;
#else
    if (!first)
        return;
    struct input_dev *d = &first->dev;
#endif
    input_event(d, type, code, (int32_t)value);
}

/* Post one event buffer. Caller has acquired eventq->lock. */
static void post(struct vinput *d, struct virtio_input_event *ev)
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
    struct vinput *d = container_of(vq->dev, struct vinput, vdev);
    if (!ev)
        return;
    if (len >= sizeof *ev)
        input_event(&d->dev, ev->type, ev->code, (int32_t)ev->value);
    post(d, ev);
}

static uint8_t cfg_select(struct vinput *d, uint8_t select, uint8_t subsel)
{
    volatile struct virtio_input_config *cfg = (volatile struct virtio_input_config *)d->vdev.device_cfg;
    cfg->select = select;
    cfg->subsel = subsel;
    return cfg->size;
}

static void probe(struct pci_dev *pci)
{
    struct vinput *d = kzalloc(sizeof *d);
    if (!d)
        return;
    if (virtio_pci_setup(pci, &d->vdev) < 0 || virtio_negotiate(&d->vdev, 0) < 0 || !d->vdev.device_cfg)
        goto fail;
    volatile struct virtio_input_config *cfg = (volatile struct virtio_input_config *)d->vdev.device_cfg;
    char name[INPUT_NAME_MAX];
    uint8_t n = cfg_select(d, VIRTIO_INPUT_CFG_ID_NAME, 0);
    unsigned len = MIN((unsigned)n, sizeof name - 1);
    for (unsigned i = 0; i < len; i++)
        name[i] = cfg->u.string[i];
    name[len] = '\0';
    input_dev_init(&d->dev, name, BUS_VIRTIO);
    n = cfg_select(d, VIRTIO_INPUT_CFG_ID_DEVIDS, 0);
    if (n >= 8) {
        d->dev.id.bustype = cfg->u.ids.bustype ? cfg->u.ids.bustype : BUS_VIRTIO;
        d->dev.id.vendor = cfg->u.ids.vendor;
        d->dev.id.product = cfg->u.ids.product;
        d->dev.id.version = cfg->u.ids.version;
    }
    n = cfg_select(d, VIRTIO_INPUT_CFG_EV_BITS, EV_KEY);
    for (unsigned code = 0; code <= KEY_MAX && code / 8 < n; code++)
        if (cfg->u.bitmap[code / 8] & (1u << (code % 8)))
            input_set_key_cap(&d->dev, code);
    n = cfg_select(d, VIRTIO_INPUT_CFG_EV_BITS, EV_REL);
    for (unsigned axis = 0; axis <= REL_MAX && axis / 8 < n; axis++)
        if (cfg->u.bitmap[axis / 8] & (1u << (axis % 8)))
            input_set_rel_cap(&d->dev, axis);
    n = cfg_select(d, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS);
    uint8_t absbits = n > 0 ? cfg->u.bitmap[0] : 0;   /* selecting ABS_INFO below replaces the bitmap */
    for (unsigned axis = 0; axis <= ABS_MAX; axis++) {
        if (!(absbits & (1u << axis)))
            continue;
        uint8_t m = cfg_select(d, VIRTIO_INPUT_CFG_ABS_INFO, (uint8_t)axis);
        if (m >= 8)
            input_set_abs_cap(&d->dev, axis, (int32_t)cfg->u.abs.min, (int32_t)cfg->u.abs.max,
                              m >= 20 ? (int32_t)cfg->u.abs.res : 0);
        else
            input_set_abs_cap(&d->dev, axis, 0, VIRTIO_INPUT_ABS_MAX, 0);
    }
    bool keyboard = (d->dev.keybit[KEY_A / 8] & (1u << (KEY_A % 8))) != 0;
    bool pointer = (d->dev.relbit & (1u << REL_X)) || (d->dev.absbit & (1u << ABS_X));
    if (!keyboard && !pointer) {
        klog_info("%s: neither keyboard nor pointer, ignored", name);
        goto fail;
    }
    if (keyboard)
        input_set_repeat(&d->dev, 500, 33);
    d->eventq = virtio_queue_setup(&d->vdev, 0, event_complete);
    if (!d->eventq) {
        klog_error("cannot set up the event queue");
        goto fail;
    }
    d->events = kzalloc(sizeof d->events[0] * INPUT_BUFFERS);
    if (!d->events)
        goto fail;
    if (input_register_device(&d->dev) < 0)
        goto fail;
    if (virtio_start(&d->vdev) < 0)
        goto fail;
    pci->driver = "virtio-input";
    spin_lock(&d->eventq->lock);
    for (unsigned i = 0; i < INPUT_BUFFERS && i < d->eventq->size / 2; i++)
        post(d, &d->events[i]);
    spin_unlock(&d->eventq->lock);
    if (!first || (pointer && !((first->dev.relbit & (1u << REL_X)) || (first->dev.absbit & (1u << ABS_X)))))
        first = d;
    if (d->dev.absbit & (1u << ABS_X))
        klog_info("%s: absolute pointer, %d..%d x %d..%d, vector %u", name, d->dev.abs[ABS_X].minimum,
                  d->dev.abs[ABS_X].maximum, d->dev.abs[ABS_Y].minimum, d->dev.abs[ABS_Y].maximum, d->vdev.vector);
    else if (pointer)
        klog_info("%s: relative pointer, vector %u", name, d->vdev.vector);
    else
        klog_info("%s: keyboard, vector %u", name, d->vdev.vector);
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
#if CONFIG_TESTS
    input_dev_init(&virtual_tablet, "virtual tablet", BUS_VIRTUAL);
    input_set_abs_cap(&virtual_tablet, ABS_X, 0, VIRTIO_INPUT_ABS_MAX, 0);
    input_set_abs_cap(&virtual_tablet, ABS_Y, 0, VIRTIO_INPUT_ABS_MAX, 0);
    input_set_rel_cap(&virtual_tablet, REL_WHEEL);
    input_set_key_cap(&virtual_tablet, BTN_LEFT);
    input_set_key_cap(&virtual_tablet, BTN_RIGHT);
    input_set_key_cap(&virtual_tablet, BTN_MIDDLE);
    input_register_device(&virtual_tablet);
#endif
}
