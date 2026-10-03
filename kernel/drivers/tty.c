#define KLOG_SUBSYS "tty"
#include <drivers/tty.h>
#include <drivers/fbcon.h>
#include <ipc/signal.h>
#include <ipc/poll.h>
#include <sched/thread.h>
#include <mm/vma.h>
#include <sched/proc.h>
#include <lib/string.h>
#include <console.h>
#include <klog.h>
#include <errno.h>

static DEFINE_WAITQ(tty_intr_waitq);     /* ttyd, condition lock console_tty.lock */

static void console_output(struct tty *t, const char *s, size_t n)
{
    console_write(s, n);
}

struct tty console_tty;

void tty_init(struct tty *t, const char *name, tty_output_fn output, bool defer_signals)
{
    memset(t, 0, sizeof *t);
    t->name = name;
    spinlock_init(&t->lock, "tty");
    waitq_init(&t->rd_waitq, "tty_rd");
    poll_source_init(&t->poll, "tty_poll");
    ring_init(&t->ready_ring, t->ready, sizeof t->ready);
    t->lflag = ICANON | ECHO | ISIG;
    t->tio.c_iflag = ICRNL | IXON;
    t->tio.c_oflag = OPOST | ONLCR;
    t->tio.c_cflag = CS8 | CREAD;
    t->tio.c_ispeed = t->tio.c_ospeed = B38400;
    static const uint8_t cc[NCCS] = { [VINTR] = 0x03, [VQUIT] = 0x1c, [VERASE] = 0x7f, [VKILL] = 0x15,
                                      [VEOF] = 0x04, [VMIN] = 1, [VSTART] = 0x11, [VSTOP] = 0x13,
                                      [VSUSP] = 0x1a, [VREPRINT] = 0x12, [VWERASE] = 0x17, [VLNEXT] = 0x16 };
    memcpy(t->tio.c_cc, cc, sizeof cc);
    t->output = output;
    t->defer_signals = defer_signals;
    t->cols = 80;
    t->rows = 25;
}

/* Caller has acquired t->lock. */
static void ready_push(struct tty *t, char c)
{
    ring_write(&t->ready_ring, &c, 1);
}

static void echo(struct tty *t, const char *s, size_t n)
{
    if ((t->lflag & ECHO) && t->output)
        t->output(t, s, n);
}

/* Line discipline with the lock acquired. Returns the signal that must be
 * delivered to the foreground group, or zero for ordinary input. */
static int input_locked(struct tty *t, char c)
{
    if (c == 0x03 && (t->lflag & ISIG)) {
        echo(t, "^C\n", 3);
        t->line_len = 0;
        return SIGINT;
    }
    if (c == 0x1a && (t->lflag & ISIG)) {
        echo(t, "^Z\n", 3);
        t->line_len = 0;
        return SIGTSTP;
    }
    if (!(t->lflag & ICANON)) {
        echo(t, &c, 1);
        ready_push(t, c);
        return false;
    }
    if (c == '\b' || c == 127) {
        if (t->line_len > 0) {
            t->line_len--;
            echo(t, "\b \b", 3);
        }
        return false;
    }
    if (c == 0x15) {   /* control U */
        while (t->line_len > 0) {
            t->line_len--;
            echo(t, "\b \b", 3);
        }
        return false;
    }
    if (c == '\n' || c == '\r') {
        echo(t, "\n", 1);
        for (size_t i = 0; i < t->line_len; i++)
            ready_push(t, t->line[i]);
        ready_push(t, '\n');
        t->line_len = 0;
        return false;
    }
    if (c == 0x04) {   /* control D: end of file when the line is empty */
        if (t->line_len == 0) {
            t->hangup = true;
        } else {
            for (size_t i = 0; i < t->line_len; i++)
                ready_push(t, t->line[i]);
            t->line_len = 0;
        }
        return false;
    }
    if (t->line_len < TTY_LINE_MAX - 1) {
        t->line[t->line_len++] = c;
        echo(t, &c, 1);
    }
    return false;
}

void tty_input_char(struct tty *t, char c)
{
    spin_lock(&t->lock);
    int sig = input_locked(t, c);
    int pgid = t->fg_pgid;
    if (sig && t->defer_signals) {
        t->signal_pending = sig;
        waitq_wake_all(&tty_intr_waitq);
    }
    waitq_wake_all(&t->rd_waitq);
    spin_unlock(&t->lock);
    poll_source_notify(&t->poll);
    if (sig && !t->defer_signals && pgid > 0)
        signal_send_pgrp(pgid, sig);
}

void tty_input_raw(struct tty *t, const char *s, size_t n)
{
    spin_lock(&t->lock);
    for (size_t i = 0; i < n; i++)
        ready_push(t, s[i]);
    waitq_wake_all(&t->rd_waitq);
    spin_unlock(&t->lock);
    poll_source_notify(&t->poll);
}

long tty_read(struct tty *t, char *buf, size_t n)
{
    if (n == 0)
        return 0;
    struct proc *p = thread_current()->proc;
    if (p != &kernel_proc) {
        spin_lock(&proc_tree_lock);
        int pgid = p->pgid;
        spin_unlock(&proc_tree_lock);
        int foreground = tty_get_fg_pgid(t);
        if (foreground > 0 && pgid != foreground) {
            signal_send_pgrp(pgid, SIGTTIN);
            return -EINTR;
        }
    }
    char tmp[TTY_LINE_MAX];
    spin_lock(&t->lock);
    while (ring_count(&t->ready_ring) == 0) {
        if (t->hangup) {
            /* A control D ends one read; the next one blocks again. */
            t->hangup = false;
            spin_unlock(&t->lock);
            return 0;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&t->lock);
            return -EINTR;
        }
        waitq_wait(&t->rd_waitq, &t->lock);
    }
    size_t got = 0;
    bool canon = (t->lflag & ICANON) != 0;
    spin_unlock(&t->lock);
    while (got < n && got < sizeof tmp) {
        char c;
        if (ring_read(&t->ready_ring, &c, 1) != 1)
            break;
        tmp[got++] = c;
        if (c == '\n' && canon)
            break;
    }
    /* User memory is copied without the lock: the copy may fault. */
    memcpy(buf, tmp, got);
    return (long)got;
}

int tty_poll(struct tty *t)
{
    spin_lock(&t->lock);
    int r = (ring_count(&t->ready_ring) || t->hangup) ? POLLIN : 0;
    spin_unlock(&t->lock);
    return r | POLLOUT;
}

struct poll_source *tty_poll_source(struct tty *t)
{
    return &t->poll;
}

int tty_getc(struct tty *t)
{
    int c = -1;
    spin_lock(&t->lock);
    uint8_t byte;
    if (ring_read(&t->ready_ring, &byte, 1) == 1)
        c = byte;
    spin_unlock(&t->lock);
    return c;
}

size_t tty_available(struct tty *t)
{
    spin_lock(&t->lock);
    size_t n = ring_count(&t->ready_ring);
    spin_unlock(&t->lock);
    return n;
}

uint32_t tty_get_lflag(struct tty *t)
{
    spin_lock(&t->lock);
    uint32_t f = t->lflag;
    spin_unlock(&t->lock);
    return f;
}

void tty_flush_input(struct tty *t)
{
    spin_lock(&t->lock);
    char c;
    while (ring_read(&t->ready_ring, &c, 1) == 1)
        ;
    t->line_len = 0;
    spin_unlock(&t->lock);
    poll_source_notify(&t->poll);
}

void tty_set_lflag(struct tty *t, uint32_t lflag)
{
    spin_lock(&t->lock);
    t->lflag = lflag;
    if (!(t->lflag & ICANON) && t->line_len) {
        for (size_t i = 0; i < t->line_len; i++)
            ready_push(t, t->line[i]);
        t->line_len = 0;
        waitq_wake_all(&t->rd_waitq);
    }
    spin_unlock(&t->lock);
    poll_source_notify(&t->poll);
}

int tty_get_fg_pgid(struct tty *t)
{
    spin_lock(&t->lock);
    int p = t->fg_pgid;
    spin_unlock(&t->lock);
    return p;
}

void tty_set_fg_pgid(struct tty *t, int pgid)
{
    spin_lock(&t->lock);
    t->fg_pgid = pgid;
    spin_unlock(&t->lock);
}

void tty_hangup(struct tty *t)
{
    spin_lock(&t->lock);
    t->hangup = true;
    waitq_wake_all(&t->rd_waitq);
    spin_unlock(&t->lock);
    poll_source_notify(&t->poll);
}

long tty_ioctl(struct tty *t, unsigned long req, uintptr_t arg)
{
    struct proc *p = thread_current()->proc;
    switch (req) {
    case TCGETS: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct termios), true))
            return -EFAULT;
        spin_lock(&t->lock);
        struct termios tm = t->tio;
        tm.c_lflag = t->lflag;
        spin_unlock(&t->lock);
        memcpy((void *)arg, &tm, sizeof tm);
        return 0;
    }
    case TCSETS: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct termios), false))
            return -EFAULT;
        struct termios tm;
        memcpy(&tm, (void *)arg, sizeof tm);
        spin_lock(&t->lock);
        t->tio = tm;
        spin_unlock(&t->lock);
        tty_set_lflag(t, tm.c_lflag);
        return 0;
    }
    case TCFLSH:
        if (arg > TCIOFLUSH)
            return -EINVAL;
        /* Output is written at once, and only input can be discarded. */
        if (arg != TCOFLUSH)
            tty_flush_input(t);
        return 0;
    case TIOCGWINSZ: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct winsize), true))
            return -EFAULT;
        struct winsize ws;
        spin_lock(&t->lock);
        ws.ws_col = t->cols;
        ws.ws_row = t->rows;
        spin_unlock(&t->lock);
        memcpy((void *)arg, &ws, sizeof ws);
        return 0;
    }
    case TIOCSWINSZ: {
        if (!vma_range_ok(p->vm, arg, sizeof(struct winsize), false))
            return -EFAULT;
        struct winsize ws;
        memcpy(&ws, (void *)arg, sizeof ws);
        spin_lock(&t->lock);
        bool changed = t->cols != ws.ws_col || t->rows != ws.ws_row;
        t->cols = ws.ws_col;
        t->rows = ws.ws_row;
        int pgid = t->fg_pgid;
        spin_unlock(&t->lock);
        if (changed && pgid > 0)
            signal_send_pgrp(pgid, SIGWINCH);
        return 0;
    }
    case TIOCGPGRP:
        return tty_get_fg_pgid(t);
    case TIOCSPGRP:
        if ((int)arg <= 0)
            return -EINVAL;
        tty_set_fg_pgid(t, (int)arg);
        return 0;
    }
    return -ENOTTY;
}

/* Console control characters arrive in the keyboard interrupt, where
 * process locks cannot be taken; this thread posts the signal. */
static void ttyd(void *arg)
{
    struct tty *t = &console_tty;
    for (;;) {
        spin_lock(&t->lock);
        while (!t->signal_pending)
            waitq_wait(&tty_intr_waitq, &t->lock);
        int sig = t->signal_pending;
        t->signal_pending = 0;
        int pgid = t->fg_pgid;
        spin_unlock(&t->lock);
        if (pgid > 0)
            signal_send_pgrp(pgid, sig);
    }
}

void tty_start_daemon(void)
{
    if (!thread_create("ttyd", ttyd, NULL, 0))
        klog_error("cannot start ttyd");
}


/* Initialized early, before the keyboard drivers. */
void console_tty_init(void)
{
    tty_init(&console_tty, "console", console_output, true);
    uint16_t cols, rows;
    fbcon_get_size(&cols, &rows);
    console_tty.cols = cols;
    console_tty.rows = rows;
}
