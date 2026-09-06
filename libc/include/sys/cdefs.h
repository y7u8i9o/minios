#pragma once

/* The BSD compiler macros that ported sources use. */
#define __BEGIN_DECLS
#define __END_DECLS
#define __dead2 __attribute__((noreturn))
#define __pure2 __attribute__((const))
#define __unused __attribute__((unused))
#define __used __attribute__((used))
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __printflike(fmt, arg) __attribute__((format(printf, fmt, arg)))
#define __unreachable() __builtin_unreachable()
#define __predict_true(x) __builtin_expect(!!(x), 1)
#define __predict_false(x) __builtin_expect(!!(x), 0)
#define __FBSDID(s)
#define __CONCAT(a, b) a ## b
#define __STRING(x) #x
