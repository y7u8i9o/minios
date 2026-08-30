#include <klog.h>
#include <sync/spinlock.h>
#include <console.h>
#include <lib/cmdline.h>
#include <lib/printf.h>
#include <lib/string.h>

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
 * by different CPUs never interleave. */
void klog_print(int level, const char *subsys, const char *fmt, ...)
{
    if (level < LOG_DEBUG || level > LOG_ERROR)
        level = LOG_ERROR;
    char line[256];
    int n = ksnprintf(line, sizeof line, "[%c %s] ", level_chars[level], subsys);
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
static char ring[KLOG_RING];
static uint64_t ring_head;          /* total bytes ever appended */
static DEFINE_SPINLOCK(ring_lock);  /* protects ring and ring_head; taken in interrupt context */
static bool ring_locked;            /* the lock is usable once the boot CPU is set up */

void klog_ring_init(void)
{
    ring_locked = true;
}

void klog_ring_append(const char *text, size_t n)
{
    if (ring_locked)
        spin_lock(&ring_lock);
    for (size_t i = 0; i < n; i++)
        ring[(ring_head + i) % KLOG_RING] = text[i];
    ring_head += n;
    if (ring_locked)
        spin_unlock(&ring_lock);
}

/* Copy up to n bytes starting at absolute offset *pos; an offset that
 * fell out of the ring is moved to the oldest byte kept. */
size_t klog_ring_read(uint64_t *pos, char *buf, size_t n)
{
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
    spin_lock(&ring_lock);
    uint64_t h = ring_head;
    spin_unlock(&ring_lock);
    return h;
}
