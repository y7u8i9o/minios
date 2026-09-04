#include <sync/spinlock.h>
#include <arch/cpu.h>
#include <mm/tlb.h>
#include <lib/string.h>
#include <debug/panic.h>

void spinlock_init(struct spinlock *lk, const char *name)
{
    lk->locked = 0;
    lk->name = name;
#if CONFIG_LOCKDEBUG
    lk->cpu = NULL;
    lk->caller = NULL;
#endif
#if CONFIG_LOCKSTAT
    lk->stat = NULL;
    lk->acquired_tsc = 0;
#endif
}

static inline bool try_acquire(struct spinlock *lk)
{
    uint32_t old = 1;
    __asm__ volatile("xchgl %0, %1" : "+r"(old), "+m"(lk->locked) : : "memory");
    return old == 0;
}

static inline void release(struct spinlock *lk)
{
    uint32_t zero = 0;
    __asm__ volatile("xchgl %0, %1" : "+r"(zero), "+m"(lk->locked) : : "memory");
}

#if CONFIG_LOCKDEBUG
static void debug_acquired(struct spinlock *lk, void *caller)
{
    lk->cpu = cpu_current();
    lk->caller = caller;
}

static void debug_check_acquire(struct spinlock *lk)
{
    if (lk->locked && lk->cpu == cpu_current())
        panic("spinlock %s: double acquire, held from %p", lk->name, lk->caller);
}

static void debug_check_release(struct spinlock *lk)
{
    if (!lk->locked)
        panic("spinlock %s: release while unlocked", lk->name);
    if (lk->cpu != cpu_current())
        panic("spinlock %s: release by non owner", lk->name);
    lk->cpu = NULL;
    lk->caller = NULL;
}
#else
static inline void debug_acquired(struct spinlock *lk, void *caller) { (void)lk; (void)caller; }
static inline void debug_check_acquire(struct spinlock *lk) { (void)lk; }
static inline void debug_check_release(struct spinlock *lk) { (void)lk; }
#endif

/* ---- statistics ---- */

#if CONFIG_LOCKSTAT
/* Counters are aggregated by name, so locks that are allocated and freed
 * (one per address space, file or inode) share one row. The table is
 * append only: a name is looked up once per lock and cached in the lock,
 * and the lookup itself runs with interrupts disabled under the lock's
 * own critical section, so a plain spinlock without statistics guards
 * the appends. */
struct lockstat lockstat_table[LOCKSTAT_MAX];
unsigned lockstat_count;
static volatile uint32_t table_lock;

static struct lockstat *stat_for(const char *name)
{
    if (!name)
        name = "?";
    unsigned n = __atomic_load_n(&lockstat_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n; i++)
        if (lockstat_table[i].name == name || strcmp(lockstat_table[i].name, name) == 0)
            return &lockstat_table[i];
    uint32_t one = 1;
    do {
        __asm__ volatile("xchgl %0, %1" : "+r"(one), "+m"(table_lock) : : "memory");
    } while (one);
    struct lockstat *st = NULL;
    for (unsigned i = 0; i < lockstat_count; i++)
        if (strcmp(lockstat_table[i].name, name) == 0)
            st = &lockstat_table[i];
    if (!st && lockstat_count < LOCKSTAT_MAX) {
        st = &lockstat_table[lockstat_count];
        st->name = name;
        __atomic_store_n(&lockstat_count, lockstat_count + 1, __ATOMIC_RELEASE);
    }
    uint32_t zero = 0;
    __asm__ volatile("xchgl %0, %1" : "+r"(zero), "+m"(table_lock) : : "memory");
    return st;
}

static inline void stat_acquired(struct spinlock *lk, bool contended, uint64_t spin_start)
{
    uint64_t now = rdtsc();
    if (!lk->stat)
        lk->stat = stat_for(lk->name);
    struct lockstat *st = lk->stat;
    if (st) {
        st->acquires++;
        if (contended) {
            st->contended++;
            st->spin_cycles += now - spin_start;
        }
    }
    lk->acquired_tsc = now;
}

static inline void stat_released(struct spinlock *lk, void *caller)
{
    struct lockstat *st = lk->stat;
    if (!st)
        return;
    uint64_t held = rdtsc() - lk->acquired_tsc;
    st->hold_cycles += held;
    if (held > st->max_hold) {
        st->max_hold = held;
        st->max_hold_caller = caller;
    }
}

void lockstat_reset(void)
{
    for (unsigned i = 0; i < lockstat_count; i++) {
        struct lockstat *st = &lockstat_table[i];
        st->acquires = st->contended = st->spin_cycles = st->hold_cycles = st->max_hold = 0;
        st->max_hold_caller = NULL;
    }
}
#else
static inline void stat_acquired(struct spinlock *lk, bool contended, uint64_t spin_start) { (void)lk; (void)contended; (void)spin_start; }
static inline void stat_released(struct spinlock *lk, void *caller) { (void)lk; (void)caller; }
void lockstat_reset(void) {}
#endif

/* Spinning happens with interrupts disabled, so a CPU waiting here cannot
 * take the TLB shootdown interrupt. It services pending shootdowns while
 * it waits, which keeps a lock holder that sends a shootdown from
 * deadlocking against a CPU that spins on that same lock. */
void spin_lock(struct spinlock *lk)
{
    push_cli();
    debug_check_acquire(lk);
    bool contended = false;
    uint64_t start = 0;
    if (!try_acquire(lk)) {
        contended = true;
#if CONFIG_LOCKSTAT
        start = rdtsc();
#endif
        do {
            cpu_relax();
            tlb_shootdown_poll();
        } while (!try_acquire(lk));
    }
    debug_acquired(lk, __builtin_return_address(0));
    stat_acquired(lk, contended, start);
}

void spin_unlock(struct spinlock *lk)
{
#if CONFIG_LOCKDEBUG && CONFIG_LOCKSTAT
    void *caller = lk->caller;
#else
    void *caller = NULL;
#endif
    debug_check_release(lk);
    stat_released(lk, caller);
    release(lk);
    pop_cli();
}

bool spin_holding(struct spinlock *lk)
{
#if CONFIG_LOCKDEBUG
    return lk->locked && lk->cpu == cpu_current();
#else
    return lk->locked != 0;
#endif
}

void spin_lock_irqsave(struct spinlock *lk, unsigned long *flags)
{
    *flags = read_rflags();
    cli();
    /* Owner tracking needs struct cpu, which the irqsave variant may
     * legitimately be used before (early console output). */
    bool contended = false;
    uint64_t start = 0;
    if (!try_acquire(lk)) {
        contended = true;
#if CONFIG_LOCKSTAT
        start = rdtsc();
#endif
        do {
            cpu_relax();
            tlb_shootdown_poll();
        } while (!try_acquire(lk));
    }
    stat_acquired(lk, contended, start);
}

void spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags)
{
    stat_released(lk, NULL);
    release(lk);
    if (flags & RFLAGS_IF)
        sti();
}
