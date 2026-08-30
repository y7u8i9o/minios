#pragma once
#include <kernel.h>

__noreturn void kassert_fail(const char *expr, const char *file, int line, const char *func);

#define kassert(expr)                                                  \
    do {                                                               \
        if (unlikely(!(expr)))                                         \
            kassert_fail(#expr, __FILE__, __LINE__, __func__);         \
    } while (0)

#define static_assert_kernel(expr, msg) _Static_assert(expr, msg)
