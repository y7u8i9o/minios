#include <syscall/syscalls.h>
#include <arch/trap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <fs/vfs.h>
#include <arch/power.h>
#include <klog.h>
#include <console.h>
#include <errno.h>
#include <drivers/timer.h>
#include <arch/smp.h>
#include <lib/string.h>
#include <minios/abi.h>




/* sleep_ms(ms): block for at least ms milliseconds. Not interruptible by
 * signals; they are delivered when the sleep ends. */
long sys_sleep_ms(struct trapframe *tf)
{
    uint64_t ms = SYSARG0(tf);
    if (ms > 3600000)
        return -EINVAL;
    sleep_ms(ms);
    return 0;
}

long sys_uptime_ms(struct trapframe *tf)
{
    return (long)timer_ms();
}

/* nproc(): number of processors. */
long sys_nproc(struct trapframe *tf)
{
    return (long)smp_cpu_count();
}

/* getcpu(): id of the processor the caller runs on at this moment. */
long sys_getcpu(struct trapframe *tf)
{
    return (long)cpu_current()->id;
}

/* uname(buf): fill a struct utsname. */
long sys_uname(struct trapframe *tf)
{
    uintptr_t addr = SYSARG0(tf);
    if (!user_range_ok(addr, sizeof(struct utsname), true))
        return -EFAULT;
    struct utsname *u = (struct utsname *)addr;
    strlcpy(u->sysname, KERNEL_SYSNAME, UTS_LEN);
    strlcpy(u->nodename, KERNEL_SYSNAME, UTS_LEN);
    strlcpy(u->release, KERNEL_RELEASE, UTS_LEN);
    strlcpy(u->version, KERNEL_VERSION, UTS_LEN);
    strlcpy(u->machine, KERNEL_MACHINE, UTS_LEN);
    return 0;
}
