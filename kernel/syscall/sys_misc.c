#include <syscall/syscalls.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <fs/vfs.h>
#include <klog.h>
#include <console.h>
#include <errno.h>
#include <drivers/timer.h>
#include <drivers/rtc.h>
#include <ipc/signal.h>
#include <arch/smp.h>
#include <lib/string.h>
#include <minios/abi.h>
#include <lib/random.h>




/* sleep_ms(ms): block until the deadline.  A signal that the thread must
 * receive, a stop and the exit of the process end the sleep with -EINTR,
 * as they end the other blocking calls (signal_should_interrupt). */
long sys_sleep_ms(struct trapframe *tf)
{
    uint64_t ms = SYSARG0(tf);
    if (ms > 3600000)
        return -EINVAL;
    return sleep_ms_interruptible(ms, signal_should_interrupt);
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
    uint64_t ns = clock == CLOCK_REALTIME ? rtc_realtime_ns() : timer_ns();
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
    if (!cred_current_is_root())
        return -EPERM;
    if (!user_range_ok(ptr, sizeof(struct timespec), false))
        return -EFAULT;
    struct timespec ts;
    memcpy(&ts, (void *)ptr, sizeof ts);
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return -EINVAL;
    rtc_set_realtime_ns((uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec);
    return 0;
}

/* The struct timeval of libc (sys/time.h): seconds and microseconds, two
 * 64 bit fields. */
struct user_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* adjtime(delta, olddelta): slew the realtime clock by delta, a struct
 * timeval, in place of the slew in progress, and store the correction that
 * remained of that slew in olddelta. Either may be NULL. Changing the
 * clock requires root. */
long sys_adjtime(struct trapframe *tf)
{
    uintptr_t dptr = SYSARG0(tf), optr = SYSARG1(tf);
    int64_t delta = 0;
    if (dptr) {
        if (!cred_current_is_root())
            return -EPERM;
        if (!user_range_ok(dptr, sizeof(struct user_timeval), false))
            return -EFAULT;
        struct user_timeval tv;
        memcpy(&tv, (void *)dptr, sizeof tv);
        if (tv.tv_usec <= -1000000 || tv.tv_usec >= 1000000 || tv.tv_sec > 2000000 || tv.tv_sec < -2000000)
            return -EINVAL;
        delta = tv.tv_sec * 1000000000 + tv.tv_usec * 1000;
    }
    if (optr && !user_range_ok(optr, sizeof(struct user_timeval), true))
        return -EFAULT;
    int64_t old = rtc_adjust(dptr ? &delta : NULL);
    if (optr) {
        /* The remaining correction in whole microseconds, both fields with
         * the sign of the correction. */
        struct user_timeval tv = { .tv_sec = old / 1000000000, .tv_usec = (old % 1000000000) / 1000 };
        memcpy((void *)optr, &tv, sizeof tv);
    }
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
    strlcpy(u->release, kernel_release, UTS_LEN);
    strlcpy(u->version, kernel_version, UTS_LEN);
    strlcpy(u->machine, KERNEL_MACHINE, UTS_LEN);
    return 0;
}

/* getrandom(buf, len, flags): bytes of the kernel generator
 * (lib/random.c), at most 256 per call. The flags GRND_NONBLOCK and
 * GRND_RANDOM change nothing: the generator never blocks after its boot
 * seed, and without one the call fails with EAGAIN. */
long sys_getrandom(struct trapframe *tf)
{
    uintptr_t buf = SYSARG0(tf);
    size_t len = SYSARG1(tf);
    unsigned flags = (unsigned)SYSARG2(tf);
    if (flags & ~(unsigned)(GRND_NONBLOCK | GRND_RANDOM))
        return -EINVAL;
    if (len > 256)
        len = 256;
    if (!user_range_ok(buf, len, true))
        return -EFAULT;
    uint8_t bytes[256];
    int r = random_bytes(bytes, len);
    if (r < 0)
        return r;
    memcpy((void *)buf, bytes, len);
    for (size_t i = 0; i < len; i++)
        ((volatile uint8_t *)bytes)[i] = 0;
    return (long)len;
}
