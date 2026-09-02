#include <debug/panic.h>
#include <debug/backtrace.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <arch/trap.h>
#include <arch/power.h>
#include <console.h>
#include <drivers/fbdev.h>
#include <drivers/debugexit.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <sched/proc.h>

volatile int panic_in_progress;

#define PANIC_EXIT_CODE 1

static __noreturn void panic_finish(void)
{
    fb_panic_flush();
#if CONFIG_PANIC_EXIT
    kprintf("panic: exiting through isa-debug-exit\n");
    debugexit_exit(PANIC_EXIT_CODE);
#endif
    kprintf("panic: system halted\n");
    cpu_halt_forever();
}

static void panic_begin(const char *fmt, va_list ap)
{
    cli();
    if (panic_in_progress) {
        /* Nested panic: print what we can and stop. */
        kprintf("\nnested panic: ");
        kvprintf(fmt, ap);
        kprintf("\n");
        panic_finish();
    }
    panic_in_progress = 1;
    smp_halt_others();
    kprintf("\n*** kernel panic: ");
    kvprintf(fmt, ap);
    kprintf(" ***\n");
    if (sched_started()) {
        struct thread *t = cpu_current()->current;
        if (t)
            kprintf("cpu %u, thread %s (tid %d), process %s (pid %d)\n",
                    cpu_current()->id, t->name, t->tid, t->proc->name, t->proc->pid);
    }
}

__noreturn void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    panic_begin(fmt, ap);
    va_end(ap);
    kprintf("backtrace:\n");
    backtrace_print();
    panic_finish();
}

__noreturn void panic_trap(struct trapframe *tf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    panic_begin(fmt, ap);
    va_end(ap);
    trap_dump_frame(tf);
    kprintf("backtrace from trap frame:\n");
    backtrace_print_from(tf->rip, tf->rbp);
    panic_finish();
}
