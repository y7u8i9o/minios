#define KLOG_SUBSYS "pty"
#include <drivers/pty.h>
#include <drivers/tty.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <ipc/signal.h>
#include <ipc/poll.h>
#include <sync/ring.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <klog.h>
#include <errno.h>

#define PTY_MAX 8
#define PTY_OUT_MAX 4096

/* One pair. lock protects the output ring (slave writes, master reads),
 * the open flags and is the condition lock of out_waitq. The line
 * discipline of the slave side is in tty. */
struct pty {
    int index;
    struct spinlock lock;
    struct waitq out_waitq;
    struct poll_source poll;
    char out[PTY_OUT_MAX];
    struct spsc_ring out_ring;
    bool master_open;
    int slave_open;                 /* number of open slave files */
    struct tty tty;
    char name[16];
};

static struct pty ptys[PTY_MAX];
static DEFINE_SPINLOCK(pty_table_lock);    /* allocation of pairs */

static void pty_output(struct tty *t, const char *s, size_t n)
{
    struct pty *p = container_of(t, struct pty, tty);
    spin_lock(&p->lock);
    ring_write(&p->out_ring, s, n);
    waitq_wake_all(&p->out_waitq);
    spin_unlock(&p->lock);
    poll_source_notify(&p->poll);
}

/* ---- master ---- */

static int ptmx_open(struct inode *ino, struct file *f)
{
    spin_lock(&pty_table_lock);
    for (int i = 0; i < PTY_MAX; i++) {
        struct pty *p = &ptys[i];
        if (!p->master_open && p->slave_open == 0) {
            p->master_open = true;
            spin_unlock(&pty_table_lock);
            tty_init(&p->tty, p->name, pty_output, false);
            ring_init(&p->out_ring, p->out, sizeof p->out);
            f->priv = p;
            klog_info("pts%d opened", i);
            return 0;
        }
    }
    spin_unlock(&pty_table_lock);
    return -ENOSPC;
}

static long ptmx_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct pty *p = f->priv;
    char tmp[256];
    spin_lock(&p->lock);
    while (ring_count(&p->out_ring) == 0) {
        if (signal_should_interrupt()) {
            spin_unlock(&p->lock);
            return -EINTR;
        }
        waitq_wait(&p->out_waitq, &p->lock);
    }
    spin_unlock(&p->lock);
    size_t got = ring_read(&p->out_ring, tmp, MIN(n, sizeof tmp));
    spin_lock(&p->lock);
    waitq_wake_all(&p->out_waitq);
    spin_unlock(&p->lock);
    memcpy(buf, tmp, got);
    return (long)got;
}

static long ptmx_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct pty *p = f->priv;
    char tmp[256];
    size_t done = 0;
    while (done < n) {
        size_t chunk = MIN(n - done, sizeof tmp);
        memcpy(tmp, buf + done, chunk);
        for (size_t i = 0; i < chunk; i++)
            tty_input_char(&p->tty, tmp[i]);
        done += chunk;
    }
    return (long)n;
}

static int ptmx_poll(struct file *f)
{
    struct pty *p = f->priv;
    spin_lock(&p->lock);
    int r = ring_count(&p->out_ring) ? POLLIN : 0;
    spin_unlock(&p->lock);
    return r | POLLOUT;
}

static struct poll_source *ptmx_poll_source(struct file *f)
{
    return &((struct pty *)f->priv)->poll;
}

static long ptmx_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    struct pty *p = f->priv;
    if (req == TIOCGPTN) {
        if (!vma_range_ok(thread_current()->proc->vm, arg, sizeof(int), true))
            return -EFAULT;
        *(int *)arg = p->index;
        return 0;
    }
    return tty_ioctl(&p->tty, req, arg);
}

static void ptmx_release(struct file *f)
{
    struct pty *p = f->priv;
    if (!p)
        return;     /* open failed, nothing was allocated */
    /* Readers of the slave see end of file, the foreground group gets
     * SIGHUP as on a real terminal. */
    int pgid = tty_get_fg_pgid(&p->tty);
    tty_hangup(&p->tty);
    if (pgid > 0)
        signal_send_pgrp(pgid, SIGHUP);
    spin_lock(&pty_table_lock);
    p->master_open = false;
    spin_unlock(&pty_table_lock);
    klog_info("pts%d master closed", p->index);
}

static const struct file_ops ptmx_fops = {
    .open = ptmx_open,
    .read = ptmx_read,
    .write = ptmx_write,
    .poll = ptmx_poll,
    .poll_source = ptmx_poll_source,
    .ioctl = ptmx_ioctl,
    .release = ptmx_release,
};

/* ---- slave ---- */

static int pts_open(struct inode *ino, struct file *f)
{
    struct pty *p = ino->priv;
    spin_lock(&pty_table_lock);
    if (!p->master_open) {
        spin_unlock(&pty_table_lock);
        return -ENXIO;
    }
    p->slave_open++;
    spin_unlock(&pty_table_lock);
    f->priv = p;
    return 0;
}

static long pts_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct pty *p = f->priv;
    return tty_read(&p->tty, buf, n);
}

static long pts_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    struct pty *p = f->priv;
    char tmp[256];
    size_t done = 0;
    while (done < n) {
        size_t chunk = MIN(n - done, sizeof tmp);
        memcpy(tmp, buf + done, chunk);
        /* Block while the master has not drained the ring. */
        spin_lock(&p->lock);
        size_t off = 0;
        while (off < chunk) {
            if (ring_space(&p->out_ring) == 0) {
                if (!p->master_open) {
                    spin_unlock(&p->lock);
                    return done ? (long)done : -EIO;
                }
                if (signal_should_interrupt()) {
                    spin_unlock(&p->lock);
                    return done ? (long)done : -EINTR;
                }
                waitq_wait(&p->out_waitq, &p->lock);
                continue;
            }
            /* Under p->lock: the echo of pty_output writes the same ring,
             * and the ring has one producer at a time. */
            off += ring_write(&p->out_ring, tmp + off, chunk - off);
        }
        waitq_wake_all(&p->out_waitq);
        spin_unlock(&p->lock);
        poll_source_notify(&p->poll);
        done += chunk;
    }
    return (long)n;
}

static int pts_poll(struct file *f)
{
    struct pty *p = f->priv;
    return tty_poll(&p->tty);
}

static struct poll_source *pts_poll_source(struct file *f)
{
    return tty_poll_source(&((struct pty *)f->priv)->tty);
}

static long pts_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    struct pty *p = f->priv;
    return tty_ioctl(&p->tty, req, arg);
}

static void pts_release(struct file *f)
{
    struct pty *p = f->priv;
    if (!p)
        return;
    spin_lock(&pty_table_lock);
    p->slave_open--;
    spin_unlock(&pty_table_lock);
}

static const struct file_ops pts_fops = {
    .open = pts_open,
    .read = pts_read,
    .write = pts_write,
    .poll = pts_poll,
    .poll_source = pts_poll_source,
    .ioctl = pts_ioctl,
    .release = pts_release,
};

void pty_init(void)
{
    devfs_register("ptmx", S_IFCHR | 0666, &ptmx_fops, NULL, 0);
    for (int i = 0; i < PTY_MAX; i++) {
        struct pty *p = &ptys[i];
        p->index = i;
        spinlock_init(&p->lock, "pty");
        ring_init(&p->out_ring, p->out, sizeof p->out);
        waitq_init(&p->out_waitq, "pty_out");
        poll_source_init(&p->poll, "pty_poll");
        ksnprintf(p->name, sizeof p->name, "pts%d", i);
        tty_init(&p->tty, p->name, pty_output, false);
        devfs_register(p->name, S_IFCHR | 0666, &pts_fops, p, 0);
    }
    klog_info("/dev/ptmx with %d slaves /dev/pts0../dev/pts%d, %zu byte output ring each",
              PTY_MAX, PTY_MAX - 1, sizeof ptys[0].out);
}
