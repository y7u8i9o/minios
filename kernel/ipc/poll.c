#include <ipc/poll.h>
#include <ipc/signal.h>
#include <fs/vfs.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <errno.h>

struct poll_waiter {
    struct spinlock lock;
    struct waitq waitq;
    bool notified;
};

struct poll_entry {
    struct list_head link;
    struct poll_source *source;
    struct poll_waiter *waiter;
};

void poll_source_init(struct poll_source *source, const char *name)
{
    spinlock_init(&source->lock, name);
    list_init(&source->waiters);
}

void poll_source_notify(struct poll_source *source)
{
    if (!source)
        return;
    spin_lock(&source->lock);
    struct list_head *pos;
    list_for_each(pos, &source->waiters) {
        struct poll_entry *entry = list_entry(pos, struct poll_entry, link);
        struct poll_waiter *waiter = entry->waiter;
        spin_lock(&waiter->lock);
        waiter->notified = true;
        waitq_wake_all(&waiter->waitq);
        spin_unlock(&waiter->lock);
    }
    spin_unlock(&source->lock);
}

static void unregister_entries(struct poll_entry *entries, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!entries[i].source)
            continue;
        spin_lock(&entries[i].source->lock);
        list_del(&entries[i].link);
        spin_unlock(&entries[i].source->lock);
    }
}

long poll_files(struct file **files, struct pollfd *pfds, size_t n, long timeout_ms)
{
    struct poll_entry *entries = n ? kzalloc(n * sizeof *entries) : NULL;
    if (n && !entries)
        return -ENOMEM;
    struct poll_waiter waiter;
    spinlock_init(&waiter.lock, "poll_waiter");
    waitq_init(&waiter.waitq, "poll_wait");
    waiter.notified = false;

    for (size_t i = 0; i < n; i++) {
        struct file *f = files[i];
        struct poll_source *source = f && f->ops && f->ops->poll_source
                                     ? f->ops->poll_source(f) : NULL;
        entries[i].source = source;
        entries[i].waiter = &waiter;
        if (source) {
            spin_lock(&source->lock);
            list_add_tail(&entries[i].link, &source->waiters);
            spin_unlock(&source->lock);
        }
    }

    uint64_t deadline = timeout_ms > 0 ? timer_ms() + (uint64_t)timeout_ms : 0;
    long result;
    for (;;) {
        spin_lock(&waiter.lock);
        waiter.notified = false;
        spin_unlock(&waiter.lock);

        long ready = 0;
        for (size_t i = 0; i < n; i++) {
            pfds[i].revents = 0;
            if (!files[i])
                continue;
            int r = files[i]->ops && files[i]->ops->poll
                    ? files[i]->ops->poll(files[i]) : POLLIN | POLLOUT;
            pfds[i].revents = (int16_t)(r & (pfds[i].events | POLLHUP | POLLERR | POLLNVAL));
            if (pfds[i].revents)
                ready++;
        }
        if (ready || timeout_ms == 0) {
            result = ready;
            break;
        }
        if (timeout_ms > 0 && timer_ms() >= deadline) {
            result = 0;
            break;
        }
        if (signal_should_interrupt()) {
            result = -EINTR;
            break;
        }

        spin_lock(&waiter.lock);
        if (!waiter.notified) {
            if (timeout_ms < 0)
                waitq_wait(&waiter.waitq, &waiter.lock);
            else
                waitq_wait_timeout(&waiter.waitq, &waiter.lock, deadline);
        }
        spin_unlock(&waiter.lock);
    }
    unregister_entries(entries, n);
    kfree(entries);
    return result;
}
