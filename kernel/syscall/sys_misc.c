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
#include <drivers/rtc.h>
#include <arch/smp.h>
#include <lib/string.h>
#include <minios/abi.h>




/* sleep_ms(ms): block until the deadline or until a signal wakes the thread. */
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

/* clock_gettime(clock, ts): CLOCK_MONOTONIC counts from boot,
 * CLOCK_REALTIME adds the epoch offset the RTC gave at boot (or the one
 * set by clock_settime). */
long sys_clock_gettime(struct trapframe *tf)
{
    uint32_t clock = (uint32_t)SYSARG0(tf);
    uintptr_t ptr = SYSARG1(tf);
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
        return -EINVAL;
    if (!user_range_ok(ptr, sizeof(struct timespec), true))
        return -EFAULT;
    uint64_t ns = timer_ns();
    if (clock == CLOCK_REALTIME)
        ns += rtc_epoch_offset_ns();
    struct timespec ts = { .tv_sec = (int64_t)(ns / 1000000000), .tv_nsec = (int64_t)(ns % 1000000000) };
    memcpy((void *)ptr, &ts, sizeof ts);
    return 0;
}

long sys_clock_settime(struct trapframe *tf)
{
    uint32_t clock = (uint32_t)SYSARG0(tf);
    uintptr_t ptr = SYSARG1(tf);
    if (clock != CLOCK_REALTIME)
        return -EINVAL;
    if (!user_range_ok(ptr, sizeof(struct timespec), false))
        return -EFAULT;
    struct timespec ts;
    memcpy(&ts, (void *)ptr, sizeof ts);
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return -EINVAL;
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
    uint64_t up = timer_ns();
    rtc_set_epoch_offset_ns(now > up ? now - up : 0);
    return 0;
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
