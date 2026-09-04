/* /dev/lockstat: the spinlock counters of CONFIG_LOCKSTAT as text, one
 * line per lock name; a write resets them. */
#define KLOG_SUBSYS "lockstat"
#include <sync/spinlock.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <debug/symbols.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <klog.h>
#include <errno.h>

static long lockstat_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
#if CONFIG_LOCKSTAT
    char line[256];
    uint64_t off = 0;
    size_t got = 0;
    int len = ksnprintf(line, sizeof line, "%-22s %12s %10s %14s %14s %12s %s\n", "NAME", "ACQUIRES", "CONTENDED",
                        "SPIN_CYCLES", "HOLD_CYCLES", "MAX_HOLD", "MAX_HOLD_CALLER");
    unsigned count = __atomic_load_n(&lockstat_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i <= count && got < n; i++) {
        if (i > 0) {
            struct lockstat st = lockstat_table[i - 1];
            const char *caller = st.max_hold_caller ? ksyms_lookup((uintptr_t)st.max_hold_caller, NULL, NULL) : NULL;
            len = ksnprintf(line, sizeof line, "%-22s %12lu %10lu %14lu %14lu %12lu %s\n", st.name, st.acquires,
                            st.contended, st.spin_cycles, st.hold_cycles, st.max_hold, caller ? caller : "-");
        }
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
#else
    (void)f; (void)buf; (void)n; (void)pos;
    return 0;
#endif
}

static long lockstat_write(struct file *f, const char *buf, size_t n, uint64_t *pos)
{
    lockstat_reset();
    return (long)n;
}

static const struct file_ops lockstat_fops = { .read = lockstat_read, .write = lockstat_write };

void lockstat_init(void)
{
    devfs_register("lockstat", S_IFCHR | 0666, &lockstat_fops, NULL, 0);
}
