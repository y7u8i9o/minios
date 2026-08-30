#include <sync/spinlock.h>
#include <arch/cpu.h>
#include <mm/tlb.h>
#include <debug/panic.h>

void spinlock_init(struct spinlock *lk, const char *name)
{
    lk->locked = 0;
    lk->name = name;
#if CONFIG_LOCKDEBUG
    lk->cpu = NULL;
    lk->caller = NULL;
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

/* Spinning happens with interrupts disabled, so a CPU waiting here cannot
 * take the TLB shootdown interrupt. It services pending shootdowns while
 * it waits, which keeps a lock holder that sends a shootdown from
 * deadlocking against a CPU that spins on that same lock. */
void spin_lock(struct spinlock *lk)
{
    push_cli();
    debug_check_acquire(lk);
    while (!try_acquire(lk)) {
        cpu_relax();
        tlb_shootdown_poll();
    }
    debug_acquired(lk, __builtin_return_address(0));
}

void spin_unlock(struct spinlock *lk)
{
    debug_check_release(lk);
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
    while (!try_acquire(lk)) {
        cpu_relax();
        tlb_shootdown_poll();
    }
}

void spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags)
{
    release(lk);
    if (flags & RFLAGS_IF)
        sti();
}
