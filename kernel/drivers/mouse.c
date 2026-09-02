#define KLOG_SUBSYS "mouse"
#include <drivers/mouse.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <ipc/signal.h>
#include <sched/wait.h>
#include <ipc/mqueue.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define MOUSE_EVENTS 64

/* The event ring. Protected by mouse_lock, taken from interrupt handlers
 * and virtqueue completions (under a virtqueue lock); condition lock of
 * mouse_waitq. */
static DEFINE_SPINLOCK(mouse_lock);
static DEFINE_WAITQ(mouse_waitq);
static struct {
    struct mouse_event ring[MOUSE_EVENTS];
    unsigned head, tail, count;
} mouse;

void mouse_push(const struct mouse_event *e)
{
    spin_lock(&mouse_lock);
    if (mouse.count < MOUSE_EVENTS) {
        struct mouse_event *slot = &mouse.ring[mouse.tail];
        *slot = *e;
        slot->time_ms = (uint32_t)timer_ms();
        mouse.tail = (mouse.tail + 1) % MOUSE_EVENTS;
        mouse.count++;
        waitq_wake_all(&mouse_waitq);
        poll_notify();
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
    while (mouse.count == 0) {
        if (signal_should_interrupt()) {
            spin_unlock(&mouse_lock);
            return -EINTR;
        }
        waitq_wait(&mouse_waitq, &mouse_lock);
    }
    size_t got = 0;
    while (got < want && mouse.count) {
        tmp[got++] = mouse.ring[mouse.head];
        mouse.head = (mouse.head + 1) % MOUSE_EVENTS;
        mouse.count--;
    }
    spin_unlock(&mouse_lock);
    memcpy(buf, tmp, got * sizeof tmp[0]);
    return (long)(got * sizeof tmp[0]);
}

static int mouse_poll(struct file *f)
{
    spin_lock(&mouse_lock);
    int r = mouse.count ? POLLIN : 0;
    spin_unlock(&mouse_lock);
    return r;
}

static const struct file_ops mouse_fops = { .read = mouse_read, .poll = mouse_poll };

void mouse_init(void)
{
    devfs_register("mouse", S_IFCHR | 0444, &mouse_fops, NULL, 0);
}
