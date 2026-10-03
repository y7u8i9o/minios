#include <console.h>
#include <lib/printf.h>
#include <drivers/serial.h>
#include <drivers/fbcon.h>
#include <drivers/fbdev.h>
#include <sync/spinlock.h>
#include <sync/mutex.h>
#include <sync/ring.h>
#include <sched/thread.h>
#include <drivers/timer.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <debug/panic.h>
#include <klog.h>

/* Serializes output to the serial port and framebuffer. Uses the irqsave
 * form because kprintf runs before struct cpu exists and inside handlers. */
static DEFINE_SPINLOCK(console_lock);
static struct mutex console_drain_mutex;

#define CONSOLE_CPU_RING 4096
struct console_cpu_ring {
    char data[CONSOLE_CPU_RING];
    struct spsc_ring ring;
    uint64_t dropped;
} __aligned(64);
static struct console_cpu_ring console_rings[MAX_CPUS];
static bool console_async;

void console_init(void)
{
    spinlock_init(&console_lock, "console");
    mutex_init(&console_drain_mutex, "console_drain");
    for (unsigned i = 0; i < MAX_CPUS; i++)
        ring_init(&console_rings[i].ring, console_rings[i].data,
                  sizeof console_rings[i].data);
}

static void console_write_unlocked(const char *s, size_t n)
{
    serial_write(s, n);
    if (fbcon_present())
        fbcon_write(s, n);
}

/* Once async output starts, consoleout is the sole serial producer. Maintain the
 * slow polled UART outside console_lock; the lock is needed only around the
 * framebuffer state shared with mode changes and the GPU flush thread. */
static void console_write_async_chunk(const char *s, size_t n)
{
    serial_write(s, n);
    unsigned long flags;
    spin_lock_irqsave(&console_lock, &flags);
    if (fbcon_present())
        fbcon_write(s, n);
    spin_unlock_irqrestore(&console_lock, flags);
}

void console_flush(void)
{
    char tmp[256];
    klog_ring_drain();
    mutex_lock(&console_drain_mutex);
    unsigned ncpu = smp_cpu_count();
    for (unsigned i = 0; i < ncpu; i++) {
        size_t n;
        while ((n = ring_read(&console_rings[i].ring, tmp, sizeof tmp)) != 0)
            console_write_async_chunk(tmp, n);
    }
    mutex_unlock(&console_drain_mutex);
}

void console_write_user(const char *s, size_t n)
{
    /* Finish queued echo and diagnostics before drawing a user's next
     * terminal update. Acquire the same mutex as consoleout for the complete
     * write so CPU migration cannot reorder successive user writes. */
    console_flush();
    mutex_lock(&console_drain_mutex);
    while (n) {
        size_t count = MIN(n, 256);
        console_write_async_chunk(s, count);
        s += count;
        n -= count;
    }
    mutex_unlock(&console_drain_mutex);
}

/* Panic path: the daemon is gone and other CPUs are halted, so write out
 * whatever the CPU rings still contain before the panic text follows it. Only
 * the serial port receives it: the daemon may have been halted in the
 * middle of a framebuffer update, and the panic text itself still reaches
 * the screen through fb_panic_flush. */
void console_panic_drain(void)
{
    char tmp[256];
    unsigned ncpu = smp_cpu_count();
    for (unsigned i = 0; i < ncpu; i++) {
        size_t n;
        while ((n = ring_read(&console_rings[i].ring, tmp, sizeof tmp)) != 0)
            serial_write(tmp, n);
    }
}

void console_write(const char *s, size_t n)
{
    /* A panicking CPU may have acquired the lock already: print without it. */
    if (panic_in_progress) {
        console_write_unlocked(s, n);
        return;
    }
    if (__atomic_load_n(&console_async, __ATOMIC_ACQUIRE)) {
        push_cli();
        struct console_cpu_ring *q = &console_rings[cpu_current()->id];
        size_t wrote = ring_write(&q->ring, s, n);
        if (wrote < n)
            q->dropped += n - wrote;
        pop_cli();
        return;
    }
    unsigned long flags;
    spin_lock_irqsave(&console_lock, &flags);
    console_write_unlocked(s, n);
    spin_unlock_irqrestore(&console_lock, flags);
}

static void consoleout(void *arg)
{
    for (;;) {
        console_flush();
        sleep_ms(1);
    }
}

void console_start_daemon(void)
{
    __atomic_store_n(&console_async, true, __ATOMIC_RELEASE);
    if (!thread_create("consoleout", consoleout, NULL, 0)) {
        __atomic_store_n(&console_async, false, __ATOMIC_RELEASE);
        console_flush();
        return;
    }
}

void console_putc(char c)
{
    console_write(&c, 1);
}

struct kprintf_state {
    char buf[128];
    size_t pos;
};

static void kprintf_flush(struct kprintf_state *st)
{
    if (st->pos) {
        console_write(st->buf, st->pos);
        st->pos = 0;
    }
}

static void kprintf_emit(char c, void *arg)
{
    struct kprintf_state *st = arg;
    st->buf[st->pos++] = c;
    if (st->pos == sizeof st->buf)
        kprintf_flush(st);
}

int kvprintf(const char *fmt, va_list ap)
{
    struct kprintf_state st;
    st.pos = 0;
    int n = kvformat(kprintf_emit, &st, fmt, ap);
    kprintf_flush(&st);
    return n;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

void console_set_fb_enabled(bool enabled)
{
    unsigned long flags;
    spin_lock_irqsave(&console_lock, &flags);
    fbcon_set_enabled(enabled);
    spin_unlock_irqrestore(&console_lock, flags);
}

void console_set_screen(const struct limine_framebuffer *screen, uint32_t scale)
{
    unsigned long flags;
    spin_lock_irqsave(&console_lock, &flags);
    fb_screen = *screen;
    fb_screen_present = true;
    fb_screen_scale = scale;
    fbcon_screen_changed();
    spin_unlock_irqrestore(&console_lock, flags);
}

bool console_take_dirty(struct fb_rect *r)
{
    unsigned long flags;
    spin_lock_irqsave(&console_lock, &flags);
    bool dirty = fbcon_take_dirty(r);
    spin_unlock_irqrestore(&console_lock, flags);
    return dirty;
}
