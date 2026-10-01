#pragma once
/* Basic definitions shared by every kernel source file. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <arch/machine.h>

#define __packed        __attribute__((packed))
#define __aligned(x)    __attribute__((aligned(x)))
#define __noreturn      __attribute__((noreturn))
#define __noinline      __attribute__((noinline))
#define __unused        __attribute__((unused))
#define __used          __attribute__((used))
#define __section(s)    __attribute__((section(s)))
#define __printf(f, a)  __attribute__((format(printf, f, a)))

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define ALIGN_UP(x, a)   ALIGN_DOWN((x) + ((__typeof__(x))(a) - 1), a)
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)
#define PAGE_MASK       (~(PAGE_SIZE - 1))

#define KiB(x)          ((x) * 1024UL)
#define MiB(x)          (KiB(x) * 1024UL)
#define GiB(x)          (MiB(x) * 1024UL)

/* Identification reported by uname. The release is the semantic version
 * in VERSION; the version string ("#build commit date") and the build
 * number come from build/kernel/version.c, written by tools/version.sh
 * at every kernel link. */
#define KERNEL_SYSNAME  "minios"
#define KERNEL_MACHINE  ARCH_MACHINE_NAME
extern const char kernel_release[];
extern const char kernel_version[];
extern const unsigned kernel_build_number;

/* The start-up sequence (init/main.c), called by the entry code of the
 * architecture. */
__noreturn void kmain(void);
