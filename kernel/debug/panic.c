#include <debug/panic.h>
#include <debug/backtrace.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <arch/frame.h>
#include <console.h>
#include <drivers/fbdev.h>
#include <arch/platform.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <sched/proc.h>

volatile int panic_in_progress;
static volatile int panic_cpu = -1;     /* the CPU writing the report */

#define PANIC_EXIT_CODE 1

static __noreturn void panic_finish(void)
{
    fb_panic_flush();
#if CONFIG_PANIC_EXIT
    kprintf("panic: ending the test run (platform_test_exit)\n");
    platform_test_exit(PANIC_EXIT_CODE);
#endif
    kprintf("panic: system halted\n");
    cpu_halt_forever();
}

static void panic_begin(const char *fmt, va_list ap)
{
    arch_irq_disable();
    if (panic_in_progress) {
        /* A CPU that faults while the first report is being written (the
         * halt IPI cannot stop a CPU already inside a fault) parks itself,
         * so the report with the frame and the backtrace still completes.
         * A nested panic on the reporting CPU prints what it can and stops. */
        if (sched_started() && (int)cpu_current()->id != panic_cpu)
            cpu_halt_forever();
        kprintf("\nnested panic: ");
        kvprintf(fmt, ap);
        kprintf("\n");
        panic_finish();
    }
    panic_in_progress = 1;
    if (sched_started())
        panic_cpu = (int)cpu_current()->id;
    smp_halt_others();
    console_panic_drain();
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
    trap_dump_extra(tf);
    kprintf("backtrace from trap frame:\n");
    backtrace_print_from(frame_pc(tf), frame_fp(tf));
    panic_finish();
}
