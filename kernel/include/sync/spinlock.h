#pragma once
#include <kernel.h>

struct cpu;

/* Spinlock. spin_lock disables interrupts through push_cli so a lock can be
 * taken from thread context and from interrupt handlers alike. Ordering
 * rules for all locks live in docs/design/locking.md. */
struct spinlock {
    volatile uint32_t locked;
    const char *name;
#if CONFIG_LOCKDEBUG
    struct cpu *cpu;        /* owning CPU while locked */
    void *caller;           /* return address of the acquiring spin_lock */
#endif
};

#if CONFIG_LOCKDEBUG
#define SPINLOCK_INIT(n) { .locked = 0, .name = (n), .cpu = NULL, .caller = NULL }
#else
#define SPINLOCK_INIT(n) { .locked = 0, .name = (n) }
#endif
#define DEFINE_SPINLOCK(var) struct spinlock var = SPINLOCK_INIT(#var)

void spinlock_init(struct spinlock *lk, const char *name);
void spin_lock(struct spinlock *lk);
void spin_unlock(struct spinlock *lk);
/* True if the current CPU holds the lock. Always true when locked without CONFIG_LOCKDEBUG. */
bool spin_holding(struct spinlock *lk);

/* Variants for data shared with interrupt handlers. They save the exact
 * RFLAGS.IF state in *flags instead of using the per CPU nesting counter,
 * which keeps them usable before struct cpu exists and inside handlers that
 * must restore the interrupted state precisely. */
void spin_lock_irqsave(struct spinlock *lk, unsigned long *flags);
void spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags);
