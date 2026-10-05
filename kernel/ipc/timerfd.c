/* timerfd: expirations counted by the timer interrupt, read as a 64 bit
 * count. timerfd_lock protects the list of armed timers and every
 * timer's fields, and is the condition lock of each timer's waitq; it
 * is taken from interrupt context, so the acquiring code disables interrupts
 * (spin_lock does). */
#include <ipc/eventfd.h>
#include <ipc/poll.h>
#include <ipc/signal.h>
#include <sched/wait.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/list.h>
#include <lib/string.h>
#include <errno.h>

struct timerfd {
    struct list_head link;
    uint64_t next_ms, interval_ms, expirations;
    bool armed;
    struct waitq waitq;
    struct poll_source poll;
};

static LIST_HEAD(timers);
static DEFINE_SPINLOCK(timerfd_lock);

void timerfd_tick(void)
{
    uint64_t now = timer_ms();
    spin_lock(&timerfd_lock);
    struct list_head *pos;
    list_for_each(pos, &timers) {
        struct timerfd *t = list_entry(pos, struct timerfd, link);
        if (!t->armed || now < t->next_ms)
            continue;
        if (t->interval_ms) {
            uint64_t n = (now - t->next_ms) / t->interval_ms + 1;
            t->expirations += n;
            t->next_ms += n * t->interval_ms;
        } else {
            t->expirations++;
            t->armed = false;
        }
        waitq_wake_all(&t->waitq);
        poll_source_notify(&t->poll);
    }
    spin_unlock(&timerfd_lock);
}

uint64_t timerfd_next_deadline(void)
{
    uint64_t first = UINT64_MAX;
    spin_lock(&timerfd_lock);
    struct list_head *pos;
    list_for_each(pos, &timers) {
        struct timerfd *t = list_entry(pos, struct timerfd, link);
        if (t->armed && t->next_ms < first)
            first = t->next_ms;
    }
    spin_unlock(&timerfd_lock);
    return first;
}

static long timerfd_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct timerfd *t = f->priv;
    if (n < 8)
        return -EINVAL;
    spin_lock(&timerfd_lock);
    while (t->expirations == 0) {
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&timerfd_lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&timerfd_lock);
            return -EINTR;
        }
        waitq_wait(&t->waitq, &timerfd_lock);
    }
    uint64_t v = t->expirations;
    t->expirations = 0;
    spin_unlock(&timerfd_lock);
    memcpy(buf, &v, 8);
    return 8;
}

static int timerfd_poll(struct file *f)
{
    struct timerfd *t = f->priv;
    spin_lock(&timerfd_lock);
    int r = t->expirations ? POLLIN : 0;
    spin_unlock(&timerfd_lock);
    return r;
}

static struct poll_source *timerfd_poll_source(struct file *f)
{
    return &((struct timerfd *)f->priv)->poll;
}

static void timerfd_release(struct file *f)
{
    struct timerfd *t = f->priv;
    spin_lock(&timerfd_lock);
    list_del(&t->link);
    spin_unlock(&timerfd_lock);
    kfree(t);
}

static const struct file_ops timerfd_fops = {
    .read = timerfd_read,
    .poll = timerfd_poll,
    .poll_source = timerfd_poll_source,
    .release = timerfd_release,
};

int timerfd_create(int flags, struct file **out)
{
    struct timerfd *t = kzalloc(sizeof *t);
    if (!t)
        return -ENOMEM;
    waitq_init(&t->waitq, "timerfd");
    poll_source_init(&t->poll, "timerfd_poll");
    struct file *f = file_alloc(NULL, &timerfd_fops, O_RDONLY | (flags & O_NONBLOCK));
    if (!f) {
        kfree(t);
        return -ENOMEM;
    }
    f->priv = t;
    spin_lock(&timerfd_lock);
    list_add_tail(&t->link, &timers);
    spin_unlock(&timerfd_lock);
    *out = f;
    return 0;
}

int timerfd_settime(struct file *f, uint64_t initial_ms, uint64_t interval_ms)
{
    if (f->ops != &timerfd_fops)
        return -EINVAL;
    struct timerfd *t = f->priv;
    spin_lock(&timerfd_lock);
    t->armed = initial_ms != 0;
    t->next_ms = timer_ms() + initial_ms;
    t->interval_ms = interval_ms;
    t->expirations = 0;
    spin_unlock(&timerfd_lock);
    return 0;
}

int timerfd_gettime(struct file *f, uint64_t *remaining_ms, uint64_t *interval_ms)
{
    if (f->ops != &timerfd_fops)
        return -EINVAL;
    struct timerfd *t = f->priv;
    spin_lock(&timerfd_lock);
    uint64_t now = timer_ms();
    *remaining_ms = t->armed ? (t->next_ms > now ? t->next_ms - now : 0) : 0;
    *interval_ms = t->armed ? t->interval_ms : 0;
    spin_unlock(&timerfd_lock);
    return 0;
}
