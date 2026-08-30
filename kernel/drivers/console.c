#include <console.h>
#include <lib/printf.h>
#include <drivers/serial.h>
#include <drivers/fbcon.h>
#include <sync/spinlock.h>
#include <debug/panic.h>

/* Serializes output to the serial port and framebuffer. Uses the irqsave
 * form because kprintf runs before struct cpu exists and inside handlers. */
static DEFINE_SPINLOCK(console_lock);

void console_init(void)
{
    spinlock_init(&console_lock, "console");
}

static void console_write_unlocked(const char *s, size_t n)
{
    serial_write(s, n);
    if (fbcon_present())
        fbcon_write(s, n);
}

void console_write(const char *s, size_t n)
{
    /* A panicking CPU may hold the lock already: print without it. */
    if (panic_in_progress) {
        console_write_unlocked(s, n);
        return;
    }
    unsigned long flags;
    spin_lock_irqsave(&console_lock, &flags);
    console_write_unlocked(s, n);
    spin_unlock_irqrestore(&console_lock, flags);
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
