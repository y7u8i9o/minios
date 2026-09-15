#define KLOG_SUBSYS "profile"
#include <debug/profile.h>
#include <debug/symbols.h>
#include <arch/trap.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <mm/slab.h>
#include <drivers/timer.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <ipc/poll.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <minios/abi.h>
#include <syscall/syscalls.h>
#include <klog.h>
#include <errno.h>

/* The collection engine.
 *
 * Every CPU owns one byte ring. Its producers are that CPU's timer
 * interrupt and the hooks running on it, which is a single producer once
 * interrupts are disabled, with one exception: an interrupt can arrive
 * while a hook is writing. A per ring busy flag turns that nesting into a
 * dropped event instead of a corrupt one, and is also what teardown waits
 * for before the storage is released.
 *
 * Records are a multiple of eight bytes and never wrap: the tail of a ring
 * is filled with a padding record when the next event does not fit before
 * the wrap point, so a reader can always read a header contiguously.
 * head and tail are monotonic byte counters masked into the buffer, so the
 * difference is the fill level without an empty-or-full ambiguity.
 *
 * Recorders never take a lock and never wake a poller: they run under the
 * run queue lock and inside interrupts. Readiness is published by
 * profile_tick from the timer interrupt, which holds nothing.
 */

#define PROF_RING_BYTES 131072u
#define PROF_RING_MASK (PROF_RING_BYTES - 1u)

struct prof_pad {               /* the first eight bytes of every record */
    uint16_t size;
    uint8_t type;
    uint8_t flags;
    uint32_t reserved;
};

struct prof_ring {
    uint8_t *data;
    uint32_t head;              /* bytes produced, owned by the producer */
    uint32_t tail;              /* bytes consumed, owned by the reader */
    uint32_t busy;              /* a producer is inside the ring */
    uint64_t dropped;
    uint64_t notify;            /* records added since the last wakeup */
} __aligned(64);

static DEFINE_SPINLOCK(prof_lock);      /* session configuration and the reader */
static struct prof_ring cpu_rings[MAX_CPUS];
static struct poll_source prof_poll_source;
static bool enabled;
static uint32_t owner_pid;      /* the process that started the session */
static struct prof_config config = {
    .divider = 1,
    .events = PROF_MASK_CPU,
    .max_depth = PROF_MAX_FRAMES,
    .alloc_min = 0,
    .io_min_ns = 0,
};
uint32_t prof_event_mask;
/* Counters shared by every CPU's recorder, so they are only ever touched
 * with atomic operations; no lock guards them. */
static uint64_t counts[PROF_EV_TYPES];
static uint64_t total_events;

/* ---- the ring ---- */

static bool ring_enter(struct prof_ring *r)
{
    return r->data && __atomic_exchange_n(&r->busy, 1, __ATOMIC_ACQ_REL) == 0;
}

static void ring_leave(struct prof_ring *r)
{
    __atomic_store_n(&r->busy, 0, __ATOMIC_RELEASE);
}

/* Append one complete record, or report that it did not fit. The caller
 * holds the ring. */
static bool ring_write(struct prof_ring *r, const struct prof_event *e)
{
    uint32_t size = e->size;
    uint32_t head = r->head;
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t offset = head & PROF_RING_MASK;
    uint32_t to_end = PROF_RING_BYTES - offset;
    uint32_t pad = to_end < size ? to_end : 0;
    if (PROF_RING_BYTES - (head - tail) < size + pad) {
        r->dropped++;
        return false;
    }
    if (pad) {
        struct prof_pad filler = { .size = (uint16_t)pad, .type = PROF_EV_PAD };
        memcpy(r->data + offset, &filler, sizeof filler);
        head += pad;
        offset = 0;
    }
    memcpy(r->data + offset, e, size);
    __atomic_store_n(&r->head, head + size, __ATOMIC_RELEASE);
    r->notify++;
    return true;
}

/* ---- recording ---- */

/* Build one event and store it in the ring of the running CPU. t owns the
 * event; when stack is true the chain is unwound from tf, or from rbp when
 * there is no trapframe. */
static void record(unsigned type, uint8_t flags, uint64_t a, uint64_t b,
                   const struct trapframe *tf, uintptr_t rbp, struct thread *t, bool stack)
{
    union {
        struct prof_event e;
        uint8_t raw[PROF_EVENT_MAX];
    } buf;
    push_cli();
    struct cpu *c = cpu_current();
    struct prof_ring *r = &cpu_rings[c->id];
    if (!ring_enter(r))
        goto out;                       /* nested inside another recorder */
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        goto leave;
    uint32_t pid = config.pid;
    uint32_t self = t && t->proc ? (uint32_t)t->proc->pid : 0;
    if (pid && self != pid)
        goto leave;
    /* The reader is never recorded unless it asked for itself: draining
     * the device is work of its own, and with the scheduler class on it
     * would feed the stream it is reading. */
    if (!pid && self && self == owner_pid)
        goto leave;
    memset(&buf.e, 0, sizeof buf.e);
    buf.e.type = (uint8_t)type;
    buf.e.flags = flags;
    buf.e.cpu = (uint16_t)c->id;
    buf.e.time_ns = timer_ns();
    buf.e.pid = t && t->proc ? (uint32_t)t->proc->pid : 0;
    buf.e.tid = t ? (uint32_t)t->tid : 0;
    buf.e.a = a;
    buf.e.b = b;
    unsigned depth = 0;
    if (stack)
        depth = prof_unwind(tf, rbp, t, buf.e.chain, config.max_depth, &buf.e.flags);
    buf.e.depth = (uint16_t)depth;
    buf.e.size = (uint16_t)(PROF_EVENT_HEADER + depth * 8u);
    /* A dropped event is counted as dropped, never as recorded, so the
     * counters describe exactly what a reader can receive. */
    if (ring_write(r, &buf.e)) {
        __atomic_fetch_add(&counts[type], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&total_events, 1, __ATOMIC_RELAXED);
    }
leave:
    ring_leave(r);
out:
    pop_cli();
}

void profile_sample(const struct trapframe *tf)
{
    if (!profile_wants(PROF_EV_SAMPLE))
        return;
    struct cpu *c = cpu_current();
    struct thread *t = c->current;
    if (!t || t == c->idle)
        return;
    uint32_t div = __atomic_load_n(&config.divider, __ATOMIC_RELAXED);
    if (div > 1 && c->ticks % div)
        return;
    record(PROF_EV_SAMPLE, 0, 0, 0, tf, 0, t, true);
}

void profile_tick(void)
{
    struct prof_ring *r = &cpu_rings[cpu_current()->id];
    if (!__atomic_load_n(&r->notify, __ATOMIC_RELAXED))
        return;
    __atomic_store_n(&r->notify, 0, __ATOMIC_RELAXED);
    poll_source_notify(&prof_poll_source);
}

void profile_leave_cpu(struct thread *prev, bool preempted)
{
    if (!prev)
        return;
    if (!(__atomic_load_n(&prof_event_mask, __ATOMIC_RELAXED) & PROF_MASK_SCHED)) {
        prev->off_cpu_ns = 0;
        return;
    }
    uint64_t now = timer_ns();
    uint64_t on_cpu = prev->on_cpu_ns ? now - prev->on_cpu_ns : 0;
    prev->off_cpu_ns = now;
    if (!profile_wants(PROF_EV_BLOCK))
        return;
    uint8_t flags = preempted ? PROF_FLAG_PREEMPT : 0;
    if (prev->state == THREAD_ZOMBIE)
        flags |= PROF_FLAG_EXIT;
    record(PROF_EV_BLOCK, flags, on_cpu, (uint64_t)prev->state, NULL,
           (uintptr_t)__builtin_frame_address(0), prev, true);
}

void profile_enter_cpu(struct thread *next)
{
    if (!next)
        return;
    uint64_t now = timer_ns();
    uint64_t off = next->off_cpu_ns ? now - next->off_cpu_ns : 0;
    uint64_t ready = next->ready_ns && next->ready_ns >= next->off_cpu_ns ? now - next->ready_ns : 0;
    next->on_cpu_ns = now;
    next->off_cpu_ns = 0;
    next->ready_ns = 0;
    if (!off || !profile_wants(PROF_EV_RUN))
        return;
    /* No chain: the stack of a thread returning to a CPU is the one its
     * block event already recorded. */
    record(PROF_EV_RUN, 0, off, ready, NULL, 0, next, false);
}

void profile_ready(struct thread *t)
{
    if (t && (__atomic_load_n(&prof_event_mask, __ATOMIC_RELAXED) & PROF_MASK_SCHED))
        __atomic_store_n(&t->ready_ns, timer_ns(), __ATOMIC_RELAXED);
}

void profile_heap(bool releasing, const void *addr, size_t size)
{
    unsigned type = releasing ? PROF_EV_FREE : PROF_EV_ALLOC;
    if (!profile_wants(type))
        return;
    /* The threshold applies to both directions: recording a free whose
     * allocation was filtered out would leave the reader with an event it
     * can do nothing with. A release of unknown size is always recorded,
     * since those are the large allocations. */
    if (size && size < config.alloc_min)
        return;
    struct cpu *c = cpu_current();
    /* A free needs no chain: the allocation it retires carries one. */
    record(type, 0, (uint64_t)(uintptr_t)addr, size, NULL,
           (uintptr_t)__builtin_frame_address(0), c->current, !releasing);
}

void profile_io(bool write, bool block_device, uint64_t bytes, uint64_t start_ns)
{
    if (!profile_wants(PROF_EV_IO))
        return;
    uint64_t latency = timer_ns() - start_ns;
    if (latency < config.io_min_ns)
        return;
    uint8_t flags = (write ? PROF_FLAG_WRITE : 0) | (block_device ? PROF_FLAG_BLOCKDEV : 0);
    record(PROF_EV_IO, flags, bytes, latency, NULL,
           (uintptr_t)__builtin_frame_address(0), cpu_current()->current, true);
}

/* ---- the device ---- */

/* The oldest record of a ring, with padding skipped, or NULL when empty.
 * The reader holds prof_lock. */
static const struct prof_event *ring_peek(struct prof_ring *r)
{
    if (!r->data)
        return NULL;
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = r->tail;
    while (tail != head) {
        const struct prof_pad *p = (const void *)(r->data + (tail & PROF_RING_MASK));
        if (p->type != PROF_EV_PAD)
            break;
        tail += p->size;
    }
    __atomic_store_n(&r->tail, tail, __ATOMIC_RELEASE);
    if (tail == head)
        return NULL;
    return (const struct prof_event *)(r->data + (tail & PROF_RING_MASK));
}

/* Events are delivered in time order: every ring is ordered by
 * construction, so repeatedly taking the oldest head merges them. */
static long profdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    unsigned cpus = smp_cpu_count();
    size_t got = 0;
    spin_lock(&prof_lock);
    for (;;) {
        const struct prof_event *oldest = NULL;
        struct prof_ring *from = NULL;
        for (unsigned i = 0; i < cpus; i++) {
            const struct prof_event *e = ring_peek(&cpu_rings[i]);
            if (e && (!oldest || e->time_ns < oldest->time_ns)) {
                oldest = e;
                from = &cpu_rings[i];
            }
        }
        if (!oldest || got + oldest->size > n)
            break;
        memcpy(buf + got, oldest, oldest->size);
        got += oldest->size;
        __atomic_store_n(&from->tail, from->tail + oldest->size, __ATOMIC_RELEASE);
    }
    spin_unlock(&prof_lock);
    return (long)got;
}

static int profdev_poll(struct file *f)
{
    unsigned cpus = smp_cpu_count();
    for (unsigned i = 0; i < cpus; i++)
        if (__atomic_load_n(&cpu_rings[i].head, __ATOMIC_ACQUIRE) !=
            __atomic_load_n(&cpu_rings[i].tail, __ATOMIC_ACQUIRE))
            return POLLIN;
    return 0;
}

static struct poll_source *profdev_source(struct file *f)
{
    return &prof_poll_source;
}

/* Stop recording and wait until no producer is inside a ring. */
static void quiesce(void)
{
    unsigned cpus = smp_cpu_count();
    __atomic_store_n(&prof_event_mask, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < cpus; i++)
        while (__atomic_load_n(&cpu_rings[i].busy, __ATOMIC_ACQUIRE) != 0)
            cpu_relax();
}

static int start_session(uint32_t pid)
{
    unsigned cpus = smp_cpu_count();
    uint8_t *fresh[MAX_CPUS] = { 0 };
    for (unsigned i = 0; i < cpus; i++) {
        if (cpu_rings[i].data)
            continue;
        fresh[i] = kmalloc(PROF_RING_BYTES);
        if (!fresh[i]) {
            for (unsigned j = 0; j < i; j++)
                kfree(fresh[j]);
            return -ENOMEM;
        }
    }
    spin_lock(&prof_lock);
    quiesce();
    for (unsigned i = 0; i < cpus; i++) {
        if (!cpu_rings[i].data)
            cpu_rings[i].data = fresh[i];
        else
            kfree(fresh[i]);
        cpu_rings[i].head = cpu_rings[i].tail = 0;
        cpu_rings[i].dropped = cpu_rings[i].notify = 0;
    }
    for (unsigned i = 0; i < PROF_EV_TYPES; i++)
        __atomic_store_n(&counts[i], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&total_events, 0, __ATOMIC_RELAXED);
    config.pid = pid;
    owner_pid = (uint32_t)thread_current()->proc->pid;
    __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);
    __atomic_store_n(&prof_event_mask, config.events, __ATOMIC_RELEASE);
    spin_unlock(&prof_lock);
    return 0;
}

static int configure(const struct prof_config *in)
{
    if (in->divider > 1000 || in->max_depth > PROF_MAX_FRAMES ||
        (in->events & ~(uint32_t)PROF_MASK_ALL))
        return -EINVAL;
    spin_lock(&prof_lock);
    if (in->divider)
        config.divider = in->divider;
    if (in->max_depth)
        config.max_depth = in->max_depth;
    if (in->events)
        config.events = in->events;
    config.alloc_min = in->alloc_min;
    config.io_min_ns = in->io_min_ns;
    if (__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        __atomic_store_n(&prof_event_mask, config.events, __ATOMIC_RELEASE);
    spin_unlock(&prof_lock);
    return 0;
}

static void fill_stats(struct prof_stats *st)
{
    unsigned cpus = smp_cpu_count();
    memset(st, 0, sizeof *st);
    spin_lock(&prof_lock);
    st->enabled = enabled;
    st->pid = config.pid;
    st->divider = config.divider;
    st->event_mask = config.events;
    st->max_depth = config.max_depth;
    st->ring = PROF_RING_BYTES;
    st->cpus = cpus;
    st->period_ns = config.divider * (1000000000u / TIMER_HZ);
    st->events = __atomic_load_n(&total_events, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < PROF_EV_TYPES; i++)
        st->counts[i] = __atomic_load_n(&counts[i], __ATOMIC_RELAXED);
    for (unsigned i = 0; i < cpus; i++) {
        struct prof_ring *r = &cpu_rings[i];
        st->dropped += __atomic_load_n(&r->dropped, __ATOMIC_RELAXED);
        st->pending += __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) -
                       __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    }
    spin_unlock(&prof_lock);
}

static long profdev_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    switch (req) {
    case PROF_START:
        return start_session((uint32_t)arg);
    case PROF_STOP:
        spin_lock(&prof_lock);
        quiesce();
        spin_unlock(&prof_lock);
        return 0;
    case PROF_SET_DIVIDER: {
        if (arg < 1 || arg > 1000)
            return -EINVAL;
        struct prof_config c = { .divider = (uint32_t)arg };
        return configure(&c);
    }
    case PROF_CONFIGURE: {
        if (!user_range_ok(arg, sizeof(struct prof_config), false))
            return -EFAULT;
        struct prof_config c;
        memcpy(&c, (const void *)arg, sizeof c);
        return configure(&c);
    }
    case PROF_GET_STATS: {
        if (!user_range_ok(arg, sizeof(struct prof_stats), true))
            return -EFAULT;
        struct prof_stats st;
        fill_stats(&st);
        memcpy((void *)arg, &st, sizeof st);
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static void profdev_release(struct file *f)
{
    uint8_t *old[MAX_CPUS] = { 0 };
    unsigned cpus = smp_cpu_count();
    spin_lock(&prof_lock);
    quiesce();
    for (unsigned i = 0; i < cpus; i++) {
        old[i] = cpu_rings[i].data;
        cpu_rings[i].data = NULL;
        cpu_rings[i].head = cpu_rings[i].tail = 0;
    }
    spin_unlock(&prof_lock);
    for (unsigned i = 0; i < cpus; i++)
        kfree(old[i]);
}

static const struct file_ops profdev_fops = {
    .read = profdev_read,
    .poll = profdev_poll,
    .poll_source = profdev_source,
    .ioctl = profdev_ioctl,
    .release = profdev_release,
};

/* /dev/ksyms: the kernel symbol table as "addr size name" lines. The
 * table is static, so a read regenerates the text from the entry that
 * covers the file position. */
static long ksymsdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    size_t count = ksyms_count();
    uint64_t off = 0;
    size_t got = 0;
    char line[160];
    for (size_t i = 0; i < count && got < n; i++) {
        uintptr_t addr;
        size_t size;
        const char *name = ksyms_entry(i, &addr, &size);
        int len = ksnprintf(line, sizeof line, "%016lx %lx %s\n", addr, size, name);
        if (off + (uint64_t)len <= *pos) {
            off += (uint64_t)len;
            continue;
        }
        size_t skip = *pos > off ? (size_t)(*pos - off) : 0;
        size_t chunk = MIN((size_t)len - skip, n - got);
        memcpy(buf + got, line + skip, chunk);
        got += chunk;
        *pos += chunk;
        off += (uint64_t)len;
    }
    return (long)got;
}

static const struct file_ops ksymsdev_fops = { .read = ksymsdev_read };

void profile_init(void)
{
    poll_source_init(&prof_poll_source, "profile_poll");
    devfs_register("profile", S_IFCHR | 0666, &profdev_fops, NULL, 0);
    devfs_register("ksyms", S_IFCHR | 0444, &ksymsdev_fops, NULL, 0);
}
