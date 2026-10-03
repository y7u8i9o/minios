#define KLOG_SUBSYS "input"
#include <debug/hung.h>
#include <input/input.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <ipc/signal.h>
#include <ipc/poll.h>
#include <sched/wait.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <mm/slab.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <klog.h>
#include <errno.h>

#define INPUT_QUEUE_ORDER 2              /* pages of events per reader */
#define INPUT_QUEUE ((PAGE_SIZE << INPUT_QUEUE_ORDER) / sizeof(struct input_event))   /* 1024, a power of two */
#define INPUT_MAX_DEVICES 16

/* One open descriptor of /dev/input/eventN. The queue is protected by
 * dev->lock; waitq and poll wake readers when a report is complete. */
struct input_reader {
    struct input_dev *dev;
    struct page *qpage;
    struct input_event *q;              /* INPUT_QUEUE entries */
    unsigned head, tail;                /* head - tail events queued */
    struct waitq waitq;
    struct poll_source poll;
    struct list_head link;
};

/* The device list, appended by input_register_device and walked by the
 * repeat thread and input_device_by_name. Condition lock of
 * repeat_waitq. */
static DEFINE_SPINLOCK(input_devices_lock);
static LIST_HEAD(input_devices);
static unsigned ndevices;
static DEFINE_WAITQ(repeat_waitq);

static inline bool bit_test(const uint8_t *bits, unsigned n)
{
    return (bits[n / 8] >> (n % 8)) & 1;
}

static inline void bit_change(uint8_t *bits, unsigned n)
{
    bits[n / 8] ^= (uint8_t)(1u << (n % 8));
}

void input_dev_init(struct input_dev *dev, const char *name, uint16_t bustype)
{
    memset(dev, 0, sizeof *dev);
    strlcpy(dev->name, name, sizeof dev->name);
    dev->id.bustype = bustype;
    spinlock_init(&dev->lock, "input_dev");
    list_init(&dev->readers);
    list_init(&dev->link);
}

void input_set_abs_cap(struct input_dev *dev, unsigned axis, int32_t minimum, int32_t maximum, int32_t resolution)
{
    if (axis > ABS_MAX)
        return;
    dev->evbit |= 1u << EV_ABS;
    dev->absbit |= 1u << axis;
    dev->abs[axis].minimum = minimum;
    dev->abs[axis].maximum = maximum;
    dev->abs[axis].resolution = resolution;
}

void input_set_repeat(struct input_dev *dev, uint32_t delay_ms, uint32_t period_ms)
{
    dev->evbit |= 1u << EV_REP;
    dev->rep[REP_DELAY] = delay_ms;
    dev->rep[REP_PERIOD] = period_ms;
}

/* ---- delivery ---- */

/* Caller has acquired dev->lock. A full queue loses everything queued so far
 * and starts again with SYN_DROPPED, which tells the reader to resync. */
static void reader_push(struct input_reader *r, const struct input_event *e)
{
    if (r->head - r->tail == INPUT_QUEUE) {
        r->head = r->tail = 0;
        struct input_event d = { .time_us = e->time_us, .type = EV_SYN, .code = SYN_DROPPED, .value = 0 };
        r->q[r->head++ % INPUT_QUEUE] = d;
    }
    r->q[r->head++ % INPUT_QUEUE] = *e;
    if (e->type == EV_SYN) {
        waitq_wake_all(&r->waitq);
        poll_source_notify(&r->poll);
    }
}

/* Caller has acquired dev->lock. */
static void deliver(struct input_dev *dev, const struct input_event *e)
{
    if (dev->grab) {
        reader_push(dev->grab, e);
        return;
    }
    struct list_head *pos;
    list_for_each(pos, &dev->readers)
        reader_push(list_entry(pos, struct input_reader, link), e);
}

void input_event(struct input_dev *dev, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event e = { .time_us = timer_ns() / 1000, .type = type, .code = code, .value = value };
    bool pass = false, to_console = false, sysrq = false;
    spin_lock(&dev->lock);
    switch (type) {
    case EV_KEY:
        if (code > KEY_MAX || !bit_test(dev->keybit, code))
            break;
        if (value == 2) {
            pass = bit_test(dev->key, code);
        } else if (bit_test(dev->key, code) != (value != 0)) {
            bit_change(dev->key, code);
            pass = true;
            if (dev->evbit & (1u << EV_REP)) {
                if (value) {
                    dev->repeat_key = code;
                    dev->repeat_active = true;
                    dev->repeat_at_ms = timer_ms() + dev->rep[REP_DELAY];
                    waitq_wake_all(&repeat_waitq);
                } else if (dev->repeat_active && dev->repeat_key == code) {
                    dev->repeat_active = false;
                }
            }
        }
        to_console = pass && !dev->grab;
        /* Alt+SysRq prints the thread table, also while a client has
         * grabbed the device (debug/hung.c). */
        sysrq = pass && code == KEY_SYSRQ && value == 1 &&
                (bit_test(dev->key, KEY_LEFTALT) || bit_test(dev->key, KEY_RIGHTALT));
        break;
    case EV_REL:
        pass = code <= REL_MAX && (dev->relbit & (1u << code)) && value != 0;
        break;
    case EV_ABS:
        if (code <= ABS_MAX && (dev->absbit & (1u << code))) {
            dev->abs[code].value = value;
            pass = true;
        }
        break;
    case EV_SYN:
        /* A report without events is not delivered. */
        pass = code != SYN_REPORT || dev->pending;
        break;
    }
    if (pass) {
        dev->pending = type != EV_SYN;
        deliver(dev, &e);
    }
    spin_unlock(&dev->lock);
    if (sysrq)
        hung_request_dump();
    if (to_console)
        input_console_key(code, value);
}

bool input_dev_grabbed(struct input_dev *dev)
{
    spin_lock(&dev->lock);
    bool r = dev->grab != NULL;
    spin_unlock(&dev->lock);
    return r;
}

bool input_key_down(struct input_dev *dev, unsigned code)
{
    if (code > KEY_MAX)
        return false;
    spin_lock(&dev->lock);
    bool r = bit_test(dev->key, code);
    spin_unlock(&dev->lock);
    return r;
}

struct input_dev *input_device_by_name(const char *name)
{
    struct input_dev *found = NULL;
    struct list_head *pos;
    spin_lock(&input_devices_lock);
    list_for_each(pos, &input_devices) {
        struct input_dev *d = list_entry(pos, struct input_dev, link);
        if (strcmp(d->name, name) == 0) {
            found = d;
            break;
        }
    }
    spin_unlock(&input_devices_lock);
    return found;
}

/* ---- key repeat ---- */

/* Emits value 2 events for the pressed key of every device with EV_REP at
 * its delay and period; the console and readers see them like presses,
 * a display server ignores them and repeats in its clients. */
static void input_repeatd(void *arg)
{
    for (;;) {
        spin_lock(&input_devices_lock);
        uint64_t now = timer_ms();
        uint64_t next = now + 1000;
        struct list_head *pos;
        list_for_each(pos, &input_devices) {
            struct input_dev *d = list_entry(pos, struct input_dev, link);
            spin_lock(&d->lock);
            bool fire = d->repeat_active && d->repeat_at_ms <= now;
            uint16_t key = d->repeat_key;
            if (fire) {
                uint32_t period = d->rep[REP_PERIOD] ? d->rep[REP_PERIOD] : 1;
                d->repeat_at_ms = now + period;
            }
            if (d->repeat_active && d->repeat_at_ms < next)
                next = d->repeat_at_ms;
            spin_unlock(&d->lock);
            if (fire) {
                input_event(d, EV_KEY, key, 2);
                input_sync(d);
            }
        }
        waitq_wait_timeout(&repeat_waitq, &input_devices_lock, next);
        spin_unlock(&input_devices_lock);
    }
}

void input_start_daemon(void)
{
    if (!thread_create("input_repeatd", input_repeatd, NULL, 0))
        klog_error("cannot start the key repeat thread");
}

/* ---- /dev/input/eventN ---- */

static int evdev_open(struct inode *ino, struct file *f)
{
    struct input_dev *dev = ino->priv;
    struct input_reader *r = kzalloc(sizeof *r);
    if (!r)
        return -ENOMEM;
    r->qpage = pmm_alloc(INPUT_QUEUE_ORDER);
    if (!r->qpage) {
        kfree(r);
        return -ENOMEM;
    }
    r->q = phys_to_virt(page_to_phys(r->qpage));
    r->dev = dev;
    waitq_init(&r->waitq, "input_rd");
    poll_source_init(&r->poll, "input_poll");
    spin_lock(&dev->lock);
    list_add_tail(&r->link, &dev->readers);
    spin_unlock(&dev->lock);
    f->priv = r;
    return 0;
}

static void evdev_release(struct file *f)
{
    struct input_reader *r = f->priv;
    struct input_dev *dev = r->dev;
    spin_lock(&dev->lock);
    list_del(&r->link);
    if (dev->grab == r)
        dev->grab = NULL;
    spin_unlock(&dev->lock);
    pmm_free(r->qpage, INPUT_QUEUE_ORDER);
    kfree(r);
}

static long evdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct input_reader *r = f->priv;
    struct input_dev *dev = r->dev;
    if (n < sizeof(struct input_event))
        return -EINVAL;
    struct input_event tmp[32];
    size_t want = MIN(n / sizeof tmp[0], ARRAY_SIZE(tmp));
    spin_lock(&dev->lock);
    while (r->head == r->tail) {
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&dev->lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&dev->lock);
            return -EINTR;
        }
        waitq_wait(&r->waitq, &dev->lock);
    }
    size_t got = 0;
    while (got < want && r->head != r->tail)
        tmp[got++] = r->q[r->tail++ % INPUT_QUEUE];
    spin_unlock(&dev->lock);
    /* User memory is copied without the lock: the copy may fault. */
    memcpy(buf, tmp, got * sizeof tmp[0]);
    return (long)(got * sizeof tmp[0]);
}

static int evdev_poll(struct file *f)
{
    struct input_reader *r = f->priv;
    spin_lock(&r->dev->lock);
    int ready = r->head != r->tail ? POLLIN : 0;
    spin_unlock(&r->dev->lock);
    return ready;
}

static struct poll_source *evdev_source(struct file *f)
{
    struct input_reader *r = f->priv;
    return &r->poll;
}

static long copy_out(uintptr_t arg, const void *src, size_t len)
{
    struct proc *p = thread_current()->proc;
    if (!vma_range_ok(p->vm, arg, len, true))
        return -EFAULT;
    memcpy((void *)arg, src, len);
    return 0;
}

static long evdev_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    struct input_reader *r = f->priv;
    struct input_dev *dev = r->dev;
    struct proc *p = thread_current()->proc;
    switch (req) {
    case EVIOCGVERSION: {
        uint32_t v = EV_VERSION;
        return copy_out(arg, &v, sizeof v);
    }
    case EVIOCGID:
        return copy_out(arg, &dev->id, sizeof dev->id);
    case EVIOCGNAME:
        return copy_out(arg, dev->name, sizeof dev->name);
    case EVIOCGCAPS: {
        struct input_caps caps = { .ev_bits = dev->evbit, .rel_bits = dev->relbit, .abs_bits = dev->absbit };
        memcpy(caps.key_bits, dev->keybit, sizeof caps.key_bits);
        return copy_out(arg, &caps, sizeof caps);
    }
    case EVIOCGKEY: {
        uint8_t key[INPUT_KEY_BYTES];
        spin_lock(&dev->lock);
        memcpy(key, dev->key, sizeof key);
        spin_unlock(&dev->lock);
        return copy_out(arg, key, sizeof key);
    }
    case EVIOCGREP: {
        uint32_t rep[REP_CNT];
        spin_lock(&dev->lock);
        memcpy(rep, dev->rep, sizeof rep);
        spin_unlock(&dev->lock);
        return copy_out(arg, rep, sizeof rep);
    }
    case EVIOCSREP: {
        if (!(dev->evbit & (1u << EV_REP)))
            return -EINVAL;
        if (!vma_range_ok(p->vm, arg, sizeof(uint32_t) * REP_CNT, false))
            return -EFAULT;
        uint32_t rep[REP_CNT];
        memcpy(rep, (void *)arg, sizeof rep);
        if (rep[REP_DELAY] < 1 || rep[REP_DELAY] > 5000 || rep[REP_PERIOD] < 1 || rep[REP_PERIOD] > 5000)
            return -EINVAL;
        spin_lock(&dev->lock);
        memcpy(dev->rep, rep, sizeof rep);
        spin_unlock(&dev->lock);
        return 0;
    }
    case EVIOCGRAB: {
        long ret = 0;
        spin_lock(&dev->lock);
        if (arg) {
            if (dev->grab && dev->grab != r)
                ret = -EBUSY;
            else
                dev->grab = r;
        } else {
            if (dev->grab != r)
                ret = -EINVAL;
            else
                dev->grab = NULL;
        }
        spin_unlock(&dev->lock);
        return ret;
    }
    }
    if (req >= EVIOCGABS(0) && req <= EVIOCGABS(ABS_MAX)) {
        unsigned axis = (unsigned)(req - EVIOCGABS(0));
        if (!(dev->absbit & (1u << axis)))
            return -EINVAL;
        struct input_absinfo info;
        spin_lock(&dev->lock);
        info = dev->abs[axis];
        spin_unlock(&dev->lock);
        return copy_out(arg, &info, sizeof info);
    }
    return -ENOTTY;
}

static const struct file_ops evdev_fops = {
    .open = evdev_open, .release = evdev_release, .read = evdev_read, .ioctl = evdev_ioctl,
    .poll = evdev_poll, .poll_source = evdev_source,
};

int input_register_device(struct input_dev *dev)
{
    spin_lock(&input_devices_lock);
    if (ndevices >= INPUT_MAX_DEVICES) {
        spin_unlock(&input_devices_lock);
        return -ENOSPC;
    }
    dev->index = ndevices++;
    list_add_tail(&dev->link, &input_devices);
    spin_unlock(&input_devices_lock);
    char node[32];
    ksnprintf(node, sizeof node, "input/event%u", dev->index);
    int r = devfs_register(node, S_IFCHR | 0444, &evdev_fops, dev, 0);
    if (r < 0)
        return r;
    const char *kind = (dev->absbit & (1u << ABS_X)) ? "absolute pointer"
                     : (dev->relbit & (1u << REL_X)) ? "relative pointer"
                     : (dev->evbit & (1u << EV_KEY)) ? "keyboard" : "device";
    klog_info("event%u: %s, %s%s", dev->index, dev->name, kind, (dev->evbit & (1u << EV_REP)) ? ", repeats" : "");
    return 0;
}

void input_init(void)
{
    devfs_register("input", S_IFDIR | 0755, NULL, NULL, 0);
}
