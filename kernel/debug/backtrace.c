#include <debug/backtrace.h>
#include <debug/symbols.h>
#include <arch/cpu.h>
#include <console.h>
#include <mm/memlayout.h>

#define BACKTRACE_MAX_FRAMES 32

static bool frame_valid(uintptr_t rbp)
{
    return rbp >= HIGHER_HALF_BASE && IS_ALIGNED(rbp, 8);
}

static void backtrace_walk(uintptr_t rbp, int depth)
{
    while (depth < BACKTRACE_MAX_FRAMES && frame_valid(rbp)) {
        uintptr_t saved_rip = ((uintptr_t *)rbp)[1];
        uintptr_t next_rbp = ((uintptr_t *)rbp)[0];
        if (saved_rip == 0)
            break;
        kprintf("  ");
        ksyms_print_addr(saved_rip);
        kprintf("\n");
        if (next_rbp <= rbp)
            break;
        rbp = next_rbp;
        depth++;
    }
}

void backtrace_print(void)
{
    backtrace_walk((uintptr_t)__builtin_frame_address(0), 0);
}

void backtrace_print_from(uintptr_t rip, uintptr_t rbp)
{
    kprintf("  ");
    ksyms_print_addr(rip);
    kprintf("\n");
    backtrace_walk(rbp, 1);
}
