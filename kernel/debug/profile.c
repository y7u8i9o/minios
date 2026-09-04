#define KLOG_SUBSYS "profile"
#include <debug/profile.h>
#include <debug/symbols.h>
#include <arch/trap.h>
#include <arch/cpu.h>
#include <arch/paging.h>
#include <mm/vmm.h>
#include <mm/slab.h>
#include <mm/memlayout.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <ipc/mqueue.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <minios/abi.h>
#include <syscall/syscalls.h>
#include <klog.h>
#include <errno.h>

#define PROF_RING 8192

/* The ring and the control state. Protected by prof_lock, which the timer
 * interrupt takes on every sample. */
static DEFINE_SPINLOCK(prof_lock);
static struct prof_sample *ring;
static uint32_t head, tail;             /* indexes into ring, head == tail: empty */
static bool enabled;
static uint32_t filter_pid;
static uint32_t divider = 1;
static uint64_t samples, dropped;

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
    if (!__atomic_load_n(&enabled, __ATOMIC_RELAXED))
        return;
    struct cpu *c = cpu_current();
    struct thread *t = c->current;
    if (!t || t == c->idle)
        return;
    uint32_t div = __atomic_load_n(&divider, __ATOMIC_RELAXED);
    if (div > 1 && c->ticks % div)
        return;
    uint32_t pid = __atomic_load_n(&filter_pid, __ATOMIC_RELAXED);
    if (pid && (uint32_t)t->proc->pid != pid)
        return;
    struct prof_sample s;
    memset(&s, 0, sizeof s);
    s.pid = (uint32_t)t->proc->pid;
    s.tid = (uint32_t)t->tid;
    s.cpu = c->id;
    s.flags = (tf->cs & 3) == 3 ? PROF_FLAG_USER : 0;
    s.chain[0] = tf->rip;
    s.depth = walk_chain(tf, t, &s);

    bool was_empty;
    spin_lock(&prof_lock);
    if (!enabled || !ring) {
        spin_unlock(&prof_lock);
        return;
    }
    uint32_t next = (head + 1) % PROF_RING;
    was_empty = head == tail;
    if (next == tail) {
        dropped++;
    } else {
        ring[head] = s;
        head = next;
        samples++;
    }
    spin_unlock(&prof_lock);
    if (was_empty)
        poll_notify();
}

static long profdev_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    size_t max = n / sizeof(struct prof_sample);
    size_t got = 0;
    spin_lock(&prof_lock);
    while (got < max && ring && tail != head) {
        memcpy(buf + got * sizeof(struct prof_sample), &ring[tail], sizeof(struct prof_sample));
        tail = (tail + 1) % PROF_RING;
        got++;
    }
    spin_unlock(&prof_lock);
    return (long)(got * sizeof(struct prof_sample));
}

static int profdev_poll(struct file *f)
{
    spin_lock(&prof_lock);
    int r = head != tail ? POLLIN : 0;
    spin_unlock(&prof_lock);
    return r;
}

static long profdev_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    switch (req) {
    case PROF_START: {
        struct prof_sample *buf = kmalloc(PROF_RING * sizeof *buf);
        spin_lock(&prof_lock);
        if (!ring) {
            if (!buf) {
                spin_unlock(&prof_lock);
                return -ENOMEM;
            }
            ring = buf;
            buf = NULL;
        }
        head = tail = 0;
        samples = dropped = 0;
        filter_pid = (uint32_t)arg;
        enabled = true;
        spin_unlock(&prof_lock);
        kfree(buf);
        return 0;
    }
    case PROF_STOP:
        spin_lock(&prof_lock);
        enabled = false;
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
        struct prof_stats st;
        spin_lock(&prof_lock);
        st.samples = samples;
        st.dropped = dropped;
        st.pending = (head + PROF_RING - tail) % PROF_RING;
        st.enabled = enabled;
        st.pid = filter_pid;
        st.divider = divider;
        st.ring = PROF_RING - 1;
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
    /* Closing the device ends the session: sampling stops and the ring is
     * released with whatever it still held. */
    spin_lock(&prof_lock);
    enabled = false;
    struct prof_sample *old = ring;
    ring = NULL;
    head = tail = 0;
    spin_unlock(&prof_lock);
    kfree(old);
}

static const struct file_ops profdev_fops = {
    .read = profdev_read,
    .poll = profdev_poll,
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
    devfs_register("profile", S_IFCHR | 0666, &profdev_fops, NULL, 0);
    devfs_register("ksyms", S_IFCHR | 0444, &ksymsdev_fops, NULL, 0);
}
