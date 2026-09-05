#define KLOG_SUBSYS "profile"
#include <debug/profile.h>
#include <debug/symbols.h>
#include <arch/trap.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <arch/paging.h>
#include <mm/vmm.h>
#include <mm/slab.h>
#include <mm/memlayout.h>
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

#define PROF_CPU_RING 1024

/* Each timer interrupt is the sole producer for its CPU ring.  Device reads
 * are the sole consumer; prof_lock only serializes session reconfiguration. */
static DEFINE_SPINLOCK(prof_lock);
struct prof_cpu_ring {
    struct prof_sample *data;
    uint32_t head, tail;
    uint64_t samples, dropped;
    unsigned active;
} __aligned(64);
static struct prof_cpu_ring cpu_rings[MAX_CPUS];
static bool enabled;
static uint32_t filter_pid;
static uint32_t divider = 1;
static struct poll_source prof_poll_source;

/* Read one 8 byte word of user memory through the page tables of vm.
 * Called with interrupts disabled from the timer; the space lock is taken
 * because another CPU may be changing the tables. */
static bool read_user_word(struct vmspace *vm, uintptr_t va, uint64_t *out)
{
    if (va > USER_TOP - 8 || (va & 7))
        return false;
    uintptr_t pa;
    if (!vmm_translate(vm, va, &pa, NULL))
        return false;
    *out = *(uint64_t *)P2V(pa);
    return true;
}

/* Walk the frame pointer chain starting at rbp. In user mode every load
 * goes through the page tables so a corrupt pointer never faults; in
 * kernel mode the chain must stay inside the current kernel stack. */
static uint32_t walk_chain(const struct trapframe *tf, struct thread *t, struct prof_sample *s)
{
    uint32_t depth = 1;
    uintptr_t rbp = tf->rbp;
    bool user = (tf->cs & 3) == 3;
    struct vmspace *vm = cpu_current()->vm;
    uintptr_t stack_top = (uintptr_t)t->kstack_top;
    uintptr_t stack_bottom = stack_top - KSTACK_SIZE;
    while (depth < PROF_MAX_FRAMES) {
        uint64_t next, ret;
        if (user) {
            if (!vm || vm == &kernel_vmspace || !read_user_word(vm, rbp + 8, &ret) || !read_user_word(vm, rbp, &next))
                break;
        } else {
            if (rbp < stack_bottom || rbp + 16 > stack_top)
                break;
            ret = *(uint64_t *)(rbp + 8);
            next = *(uint64_t *)rbp;
        }
        if (!ret)
            break;
        s->chain[depth++] = ret;
        if (next <= rbp)
            break;              /* frames grow towards higher addresses */
        rbp = next;
    }
    return depth;
}

void profile_sample(const struct trapframe *tf)
{
    struct cpu *c = cpu_current();
    struct prof_cpu_ring *r = &cpu_rings[c->id];
    __atomic_fetch_add(&r->active, 1, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        goto out;
    struct thread *t = c->current;
    if (!t || t == c->idle)
        goto out;
    uint32_t div = __atomic_load_n(&divider, __ATOMIC_RELAXED);
    if (div > 1 && c->ticks % div)
        goto out;
    uint32_t pid = __atomic_load_n(&filter_pid, __ATOMIC_RELAXED);
    if (pid && (uint32_t)t->proc->pid != pid)
        goto out;
    struct prof_sample s;
    memset(&s, 0, sizeof s);
    s.pid = (uint32_t)t->proc->pid;
    s.tid = (uint32_t)t->tid;
    s.cpu = c->id;
    s.flags = (tf->cs & 3) == 3 ? PROF_FLAG_USER : 0;
    s.chain[0] = tf->rip;
    s.depth = walk_chain(tf, t, &s);

    struct prof_sample *data = __atomic_load_n(&r->data, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) || !data)
        goto out;
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1) & (PROF_CPU_RING - 1);
    bool was_empty = head == tail;
    if (next == tail) {
        __atomic_fetch_add(&r->dropped, 1, __ATOMIC_RELAXED);
    } else {
        data[head] = s;
        __atomic_store_n(&r->head, next, __ATOMIC_RELEASE);
        __atomic_fetch_add(&r->samples, 1, __ATOMIC_RELAXED);
    }
    if (was_empty)
        poll_source_notify(&prof_poll_source);
out:
    __atomic_fetch_sub(&r->active, 1, __ATOMIC_RELEASE);
}

static long profdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    size_t max = n / sizeof(struct prof_sample);
    size_t got = 0;
    unsigned cpus = smp_cpu_count();
    for (unsigned cpu = 0; cpu < cpus && got < max; cpu++) {
        struct prof_cpu_ring *r = &cpu_rings[cpu];
        struct prof_sample *data = __atomic_load_n(&r->data, __ATOMIC_ACQUIRE);
        if (!data)
            continue;
        uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
        uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        while (got < max && tail != head) {
            struct prof_sample sample = data[tail];
            tail = (tail + 1) & (PROF_CPU_RING - 1);
            __atomic_store_n(&r->tail, tail, __ATOMIC_RELEASE);
            memcpy(buf + got * sizeof sample, &sample, sizeof sample);
            got++;
        }
    }
    return (long)(got * sizeof(struct prof_sample));
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

static long profdev_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    switch (req) {
    case PROF_START: {
        unsigned cpus = smp_cpu_count();
        struct prof_sample *fresh[MAX_CPUS] = { 0 };
        for (unsigned i = 0; i < cpus; i++) {
            if (!cpu_rings[i].data) {
                fresh[i] = kmalloc(PROF_CPU_RING * sizeof *fresh[i]);
                if (!fresh[i]) {
                    for (unsigned j = 0; j < i; j++)
                        kfree(fresh[j]);
                    return -ENOMEM;
                }
            }
        }
        spin_lock(&prof_lock);
        __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < cpus; i++) {
            if (!cpu_rings[i].data)
                __atomic_store_n(&cpu_rings[i].data, fresh[i], __ATOMIC_RELEASE);
            else
                kfree(fresh[i]);
            cpu_rings[i].head = cpu_rings[i].tail = 0;
            cpu_rings[i].samples = cpu_rings[i].dropped = 0;
        }
        filter_pid = (uint32_t)arg;
        __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);
        spin_unlock(&prof_lock);
        return 0;
    }
    case PROF_STOP:
        spin_lock(&prof_lock);
        __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
        spin_unlock(&prof_lock);
        return 0;
    case PROF_SET_DIVIDER:
        if (arg < 1 || arg > 1000)
            return -EINVAL;
        __atomic_store_n(&divider, (uint32_t)arg, __ATOMIC_RELAXED);
        return 0;
    case PROF_GET_STATS: {
        if (!user_range_ok(arg, sizeof(struct prof_stats), true))
            return -EFAULT;
        struct prof_stats st = { 0 };
        spin_lock(&prof_lock);
        st.enabled = enabled;
        st.pid = filter_pid;
        st.divider = divider;
        unsigned cpus = smp_cpu_count();
        for (unsigned i = 0; i < cpus; i++) {
            struct prof_cpu_ring *r = &cpu_rings[i];
            st.samples += __atomic_load_n(&r->samples, __ATOMIC_RELAXED);
            st.dropped += __atomic_load_n(&r->dropped, __ATOMIC_RELAXED);
            uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
            uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
            st.pending += (head + PROF_CPU_RING - tail) & (PROF_CPU_RING - 1);
        }
        st.ring = PROF_CPU_RING * cpus - cpus;
        spin_unlock(&prof_lock);
        memcpy((void *)arg, &st, sizeof st);
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static void profdev_release(struct file *f)
{
    struct prof_sample *old[MAX_CPUS] = { 0 };
    spin_lock(&prof_lock);
    __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
    unsigned cpus = smp_cpu_count();
    spin_unlock(&prof_lock);
    for (unsigned i = 0; i < cpus; i++)
        while (__atomic_load_n(&cpu_rings[i].active, __ATOMIC_ACQUIRE) != 0)
            cpu_relax();
    spin_lock(&prof_lock);
    for (unsigned i = 0; i < cpus; i++) {
        old[i] = cpu_rings[i].data;
        __atomic_store_n(&cpu_rings[i].data, NULL, __ATOMIC_RELEASE);
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
