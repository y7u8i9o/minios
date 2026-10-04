#pragma once
#include <syscall_nums.h>

long __syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
/* Convert a negative errno return into -1 with errno set. */
long __syscall_ret(long r);

#define syscall0(n)                __syscall_ret(__syscall6((n), 0, 0, 0, 0, 0, 0))
#define syscall1(n, a)             __syscall_ret(__syscall6((n), (long)(a), 0, 0, 0, 0, 0))
#define syscall2(n, a, b)          __syscall_ret(__syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0))
#define syscall3(n, a, b, c)       __syscall_ret(__syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0))
#define syscall4(n, a, b, c, d)    __syscall_ret(__syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0))
#define syscall5(n, a, b, c, d, e) __syscall_ret(__syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0))
