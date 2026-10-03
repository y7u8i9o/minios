#include <klog.h>
#include <sync/spinlock.h>
#include <console.h>
#include <lib/cmdline.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <ipc/poll.h>
#include <sync/ring.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <drivers/timer.h>

int klog_runtime_level = CONFIG_LOG_LEVEL;

static const char level_chars[] = { 'D', 'I', 'W', 'E' };

void klog_set_level(int level)
{
    if (level < LOG_DEBUG)
        level = LOG_DEBUG;
    if (level > LOG_ERROR)
        level = LOG_ERROR;
    klog_runtime_level = level;
}

/* A line is formatted completely before it is written, so lines logged
 * by different CPUs never interleave. The prefix is the time since the
 * kernel entry in seconds with microseconds, then the level and the
 * subsystem: "[    0.123456] [I pmm] ...". */
void klog_print(int level, const char *subsys, const char *fmt, ...)
{
    if (level < LOG_DEBUG || level > LOG_ERROR)
        level = LOG_ERROR;
    char line[384];
    uint64_t us = timer_ns() / 1000;
    int n = ksnprintf(line, sizeof line, "[%5lu.%06lu] [%c %s] ", us / 1000000, us % 1000000,
                      level_chars[level], subsys);
    if (n < 0)
        n = 0;
    if ((size_t)n < sizeof line) {
        va_list ap;
        va_start(ap, fmt);
        int m = kvsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
        va_end(ap);
        if (m > 0)
            n += m;
    }
    if ((size_t)n > sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    line[n] = '\0';
    console_write(line, (size_t)n);
    klog_ring_append(line, (size_t)n);
}

/* ---- the kernel log ring read through /dev/klog ---- */

#define KLOG_RING 16384
#define KLOG_STAGE 16384
static char ring[KLOG_RING];
static uint64_t ring_head;          /* total bytes ever appended */
static DEFINE_SPINLOCK(ring_lock);  /* protects ring and ring_head; taken in interrupt context */
static bool ring_locked;            /* the lock is usable once the boot CPU is set up */
static struct poll_source ring_poll;
struct klog_stage {
    char data[KLOG_STAGE];
    struct spsc_ring ring;
    uint64_t dropped;
} __aligned(64);
static struct klog_stage stages[MAX_CPUS];

void klog_ring_init(void)
{
    poll_source_init(&ring_poll, "klog_poll");
    for (unsigned i = 0; i < MAX_CPUS; i++)
        ring_init(&stages[i].ring, stages[i].data, sizeof stages[i].data);
    ring_locked = true;
}

void klog_ring_append(const char *text, size_t n)
{
    if (!ring_locked) {
        for (size_t i = 0; i < n; i++)
            ring[(ring_head + i) % KLOG_RING] = text[i];
        ring_head += n;
        return;
    }
    push_cli();
    struct klog_stage *s = &stages[cpu_current()->id];
    size_t wrote = ring_write(&s->ring, text, n);
    if (wrote < n)
        s->dropped += n - wrote;
    pop_cli();
}

void klog_ring_drain(void)
{
    if (!ring_locked)
        return;
    char tmp[256];
    bool appended = false;
    spin_lock(&ring_lock);
    unsigned cpus = smp_cpu_count();
    for (unsigned cpu = 0; cpu < cpus; cpu++) {
        size_t n;
        while ((n = ring_read(&stages[cpu].ring, tmp, sizeof tmp)) != 0) {
            for (size_t i = 0; i < n; i++)
                ring[(ring_head + i) % KLOG_RING] = tmp[i];
            ring_head += n;
            appended = true;
        }
    }
    spin_unlock(&ring_lock);
    if (appended)
        poll_source_notify(&ring_poll);
}

/* Copy up to n bytes starting at absolute offset *pos; an offset that
 * fell out of the ring is moved to the oldest byte retained. */
size_t klog_ring_read(uint64_t *pos, char *buf, size_t n)
{
    klog_ring_drain();
    spin_lock(&ring_lock);
    uint64_t oldest = ring_head > KLOG_RING ? ring_head - KLOG_RING : 0;
    if (*pos < oldest)
        *pos = oldest;
    size_t avail = (size_t)(ring_head - *pos);
    if (n > avail)
        n = avail;
    for (size_t i = 0; i < n; i++)
        buf[i] = ring[(*pos + i) % KLOG_RING];
    *pos += n;
    spin_unlock(&ring_lock);
    return n;
}

uint64_t klog_ring_head(void)
{
    klog_ring_drain();
    spin_lock(&ring_lock);
    uint64_t h = ring_head;
    spin_unlock(&ring_lock);
    return h;
}

struct poll_source *klog_poll_source(void)
{
    return &ring_poll;
}
