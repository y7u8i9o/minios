/* Frame pointer unwinding for the profiler.
 *
 * Kernel frames are read directly, after every frame pointer has been
 * checked against the kernel stack of the thread that owns it, so a
 * corrupt chain ends the walk instead of faulting. User frames are read
 * through the page tables of the address space with vmm_translate, which
 * cannot fault either and works while the sampled space is not the one
 * loaded in CR3.
 *
 * A kernel chain continues into the user frames of the entry that led into
 * the kernel, separated by PROF_FRAME_BOUNDARY, so one chain shows which
 * user code is responsible for the kernel work. The entry frame is the one
 * the thread itself recorded at its innermost user to kernel transition;
 * it is accepted only when it lies inside that thread's kernel stack and
 * came from ring three, so a stale pointer is ignored rather than trusted.
 */
#include <debug/profile.h>
#include <arch/trap.h>
#include <arch/cpu.h>
#include <arch/paging.h>
#include <mm/vmm.h>
#include <mm/memlayout.h>
#include <sched/thread.h>
#include <sched/proc.h>

/* Read one aligned word of user memory through the tables of vm. */
static bool read_user_word(struct vmspace *vm, uintptr_t va, uint64_t *out)
{
    if (!vm || vm == &kernel_vmspace || va > USER_TOP - 8 || (va & 7))
        return false;
    uintptr_t pa;
    if (!vmm_translate(vm, va, &pa, NULL))
        return false;
    *out = *(uint64_t *)P2V(pa);
    return true;
}

static unsigned walk_user(struct vmspace *vm, uintptr_t rip, uintptr_t rbp,
                          uint64_t *chain, unsigned max)
{
    unsigned depth = 0;
    if (max && rip) {
        chain[depth++] = rip;
    }
    while (depth < max) {
        uint64_t next, ret;
        if (!read_user_word(vm, rbp + 8, &ret) || !read_user_word(vm, rbp, &next) || !ret)
            break;
        chain[depth++] = ret;
        if (next <= rbp)
            break;              /* frames grow towards higher addresses */
        rbp = next;
    }
    return depth;
}

/* The kernel stack of t, or the range covering rbp when the thread runs on
 * a boot or idle stack whose extent is not recorded. */
static bool kernel_bounds(const struct thread *t, uintptr_t *bottom, uintptr_t *top)
{
    if (!t || !t->kstack_top)
        return false;
    *top = (uintptr_t)t->kstack_top;
    *bottom = *top - KSTACK_SIZE;
    return true;
}

static unsigned walk_kernel(uintptr_t rip, uintptr_t rbp, const struct thread *t,
                            uint64_t *chain, unsigned max)
{
    uintptr_t bottom, top;
    unsigned depth = 0;
    if (max && rip)
        chain[depth++] = rip;
    if (!kernel_bounds(t, &bottom, &top))
        return depth;
    while (depth < max) {
        if (rbp < bottom || rbp + 16 > top)
            break;
        uint64_t ret = *(uint64_t *)(rbp + 8);
        uint64_t next = *(uint64_t *)rbp;
        if (!ret)
            break;
        chain[depth++] = ret;
        if (next <= rbp)
            break;
        rbp = next;
    }
    return depth;
}

/* The innermost user entry frame of t, or NULL. */
static const struct trapframe *entry_frame(const struct thread *t)
{
    uintptr_t bottom, top;
    const struct trapframe *tf = __atomic_load_n(&t->kentry_frame, __ATOMIC_RELAXED);
    if (!tf || !kernel_bounds(t, &bottom, &top))
        return NULL;
    uintptr_t at = (uintptr_t)tf;
    if (at < bottom || at + sizeof *tf > top || (tf->cs & 3) != 3)
        return NULL;
    return tf;
}

unsigned prof_unwind(const struct trapframe *tf, uintptr_t rbp, struct thread *t,
                     uint64_t *chain, unsigned max, uint8_t *flags)
{
    struct vmspace *vm = t && t->proc ? t->proc->vm : NULL;
    if (max > PROF_MAX_FRAMES)
        max = PROF_MAX_FRAMES;
    if (tf && (tf->cs & 3) == 3) {
        *flags |= PROF_FLAG_USER;
        unsigned depth = walk_user(vm, tf->rip, tf->rbp, chain, max);
        if (depth == max)
            *flags |= PROF_FLAG_TRUNC;
        return depth;
    }
    unsigned depth = walk_kernel(tf ? tf->rip : 0, tf ? tf->rbp : rbp, t, chain, max);
    if (depth == max) {
        *flags |= PROF_FLAG_TRUNC;
        return depth;
    }
    /* Continue with the user frames of the entry into the kernel. The
     * boundary costs one slot, so it is only added when frames follow. */
    const struct trapframe *entry = t ? entry_frame(t) : NULL;
    if (!entry || depth + 2 > max)
        return depth;
    unsigned user = walk_user(vm, entry->rip, entry->rbp, chain + depth + 1, max - depth - 1);
    if (!user)
        return depth;
    chain[depth] = PROF_FRAME_BOUNDARY;
    depth += 1 + user;
    *flags |= PROF_FLAG_KUSER;
    if (depth == max)
        *flags |= PROF_FLAG_TRUNC;
    return depth;
}
