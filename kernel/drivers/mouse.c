#define KLOG_SUBSYS "mouse"
#include <drivers/mouse.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <ipc/signal.h>
#include <sched/wait.h>
#include <ipc/poll.h>
#include <sync/ring.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define MOUSE_EVENTS 64

/* The event ring. Protected by mouse_lock, taken from interrupt handlers
 * and virtqueue completions (under a virtqueue lock); condition lock of
 * mouse_waitq. */
static DEFINE_SPINLOCK(mouse_lock);
static DEFINE_WAITQ(mouse_waitq);
static struct poll_source mouse_poll_source;
static struct {
    struct mouse_event ring[MOUSE_EVENTS];
    struct spsc_ring bytes;
} mouse;

void mouse_push(const struct mouse_event *e)
{
    spin_lock(&mouse_lock);
    if (ring_space(&mouse.bytes) >= sizeof *e) {
        struct mouse_event event = *e;
        event.time_ms = (uint32_t)timer_ms();
        ring_write(&mouse.bytes, &event, sizeof event);
        waitq_wake_all(&mouse_waitq);
        poll_source_notify(&mouse_poll_source);
    }
    spin_unlock(&mouse_lock);
}

static long mouse_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    if (n < sizeof(struct mouse_event))
        return -EINVAL;
    struct mouse_event tmp[8];
    size_t want = MIN(n / sizeof tmp[0], ARRAY_SIZE(tmp));
    spin_lock(&mouse_lock);
    while (ring_count(&mouse.bytes) < sizeof(struct mouse_event)) {
        if (signal_should_interrupt()) {
            spin_unlock(&mouse_lock);
            return -EINTR;
        }
        waitq_wait(&mouse_waitq, &mouse_lock);
    }
    spin_unlock(&mouse_lock);
    size_t got = ring_read(&mouse.bytes, tmp, want * sizeof tmp[0]) / sizeof tmp[0];
    memcpy(buf, tmp, got * sizeof tmp[0]);
    return (long)(got * sizeof tmp[0]);
}

static int mouse_poll(struct file *f)
{
    spin_lock(&mouse_lock);
    int r = ring_count(&mouse.bytes) >= sizeof(struct mouse_event) ? POLLIN : 0;
    spin_unlock(&mouse_lock);
    return r;
}

static struct poll_source *mouse_source(struct file *f)
{
    return &mouse_poll_source;
}

static const struct file_ops mouse_fops = {
    .read = mouse_read, .poll = mouse_poll, .poll_source = mouse_source
};

void mouse_init(void)
{
    ring_init(&mouse.bytes, mouse.ring, sizeof mouse.ring);
    poll_source_init(&mouse_poll_source, "mouse_poll");
    devfs_register("mouse", S_IFCHR | 0444, &mouse_fops, NULL, 0);
}
