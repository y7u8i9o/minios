#define KLOG_SUBSYS "syscall"
#include <syscall/syscalls.h>
#include <syscall_nums.h>
#include <arch/syscall.h>
#include <arch/cpu.h>
#include <arch/frame.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

static const syscall_fn syscall_table[SYS_MAX] = {
    [SYS_write]         = sys_write,
    [SYS_exit]          = sys_exit,
    [SYS_getpid]        = sys_getpid,
    [SYS_yield]         = sys_yield,
    [SYS_fork]          = sys_fork,
    [SYS_execve]        = sys_execve,
    [SYS_wait4]         = sys_wait4,
    [SYS_kill]          = sys_kill,
    [SYS_getppid]       = sys_getppid,
    [SYS_thread_create] = sys_thread_create,
    [SYS_thread_exit]   = sys_thread_exit,
    [SYS_thread_join]   = sys_thread_join,
    [SYS_read]          = sys_read,
    [SYS_sbrk]          = sys_sbrk,
    [SYS_chdir]         = sys_chdir,
    [SYS_getcwd]        = sys_getcwd,
    [SYS_open]          = sys_open,
    [SYS_close]         = sys_close,
    [SYS_lseek]         = sys_lseek,
    [SYS_dup]           = sys_dup,
    [SYS_dup2]          = sys_dup2,
    [SYS_stat]          = sys_stat,
    [SYS_fstat]         = sys_fstat,
    [SYS_getdents]      = sys_getdents,
    [SYS_mkdir]         = sys_mkdir,
    [SYS_unlink]        = sys_unlink,
    [SYS_rmdir]         = sys_rmdir,
    [SYS_rename]        = sys_rename,
    [SYS_link]          = sys_link,
    [SYS_pipe]          = sys_pipe,
    [SYS_mount]         = sys_mount,
    [SYS_umount]        = sys_umount,
    [SYS_sync]          = sys_sync,
    [SYS_reboot]        = sys_reboot,
    [SYS_sigaction]     = sys_sigaction,
    [SYS_sigprocmask]   = sys_sigprocmask,
    [SYS_sigreturn]     = sys_sigreturn,
    [SYS_setpgid]       = sys_setpgid,
    [SYS_getpgid]       = sys_getpgid,
    [SYS_tcsetpgrp]     = sys_tcsetpgrp,
    [SYS_tcgetpgrp]     = sys_tcgetpgrp,
    [SYS_ioctl]         = sys_ioctl,
    [SYS_sleep_ms]      = sys_sleep_ms,
    [SYS_mq_open]       = sys_mq_open,
    [SYS_mq_unlink]     = sys_mq_unlink,
    [SYS_shm_open]      = sys_shm_open,
    [SYS_shm_unlink]    = sys_shm_unlink,
    [SYS_poll]          = sys_poll,
    [SYS_uptime_ms]     = sys_uptime_ms,
    [SYS_nproc]         = sys_nproc,
    [SYS_getcpu]        = sys_getcpu,
    [SYS_uname]         = sys_uname,
    [SYS_socket]        = sys_socket,
    [SYS_socketpair]    = sys_socketpair,
    [SYS_bind]          = sys_bind,
    [SYS_listen]        = sys_listen,
    [SYS_accept]        = sys_accept,
    [SYS_connect]       = sys_connect,
    [SYS_sendmsg]       = sys_sendmsg,
    [SYS_recvmsg]       = sys_recvmsg,
    [SYS_shutdown]      = sys_shutdown,
    [SYS_memfd_create]  = sys_memfd_create,
    [SYS_ftruncate]     = sys_ftruncate,
    [SYS_eventfd]       = sys_eventfd,
    [SYS_timerfd_create] = sys_timerfd_create,
    [SYS_timerfd_settime] = sys_timerfd_settime,
    [SYS_timerfd_gettime] = sys_timerfd_gettime,
    [SYS_set_tls]       = sys_set_tls,
    [SYS_gettid]        = sys_gettid,
    [SYS_futex]         = sys_futex,
    [SYS_clock_gettime] = sys_clock_gettime,
    [SYS_clock_settime] = sys_clock_settime,
    [SYS_fcntl]         = sys_fcntl,
    [SYS_pipe2]         = sys_pipe2,
    [SYS_mmap]          = sys_mmap,
    [SYS_munmap]        = sys_munmap,
    [SYS_mprotect]      = sys_mprotect,
    [SYS_msync]         = sys_msync,
    [SYS_madvise]       = sys_madvise,
    [SYS_getrlimit]     = sys_getrlimit,
    [SYS_setrlimit]     = sys_setrlimit,
    [SYS_prlimit]       = sys_prlimit,
    [SYS_getrusage]     = sys_getrusage,
    [SYS_utimensat]     = sys_utimensat,
    [SYS_openat]        = sys_openat,
    [SYS_fstatat]       = sys_fstatat,
    [SYS_getsockname]   = sys_getsockname,
    [SYS_getpeername]   = sys_getpeername,
    [SYS_setsockopt]    = sys_setsockopt,
    [SYS_getsockopt]    = sys_getsockopt,
    [SYS_symlink]       = sys_symlink,
    [SYS_symlinkat]     = sys_symlinkat,
    [SYS_readlink]      = sys_readlink,
    [SYS_readlinkat]    = sys_readlinkat,
    [SYS_lstat]         = sys_lstat,
    [SYS_getresuid]     = sys_getresuid,
    [SYS_getresgid]     = sys_getresgid,
    [SYS_setresuid]     = sys_setresuid,
    [SYS_setresgid]     = sys_setresgid,
    [SYS_getgroups]     = sys_getgroups,
    [SYS_setgroups]     = sys_setgroups,
    [SYS_umask]         = sys_umask,
    [SYS_fchmodat]      = sys_fchmodat,
    [SYS_fchmod]        = sys_fchmod,
    [SYS_fchownat]      = sys_fchownat,
    [SYS_fchown]        = sys_fchown,
};

bool user_range_ok(uintptr_t addr, size_t len, bool write)
{
    struct proc *p = thread_current()->proc;
    if (!p->vm)
        return false;
    return vma_range_ok(p->vm, addr, len, write);
}

long copy_string_from_user(char *dst, uintptr_t src, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if (!user_range_ok(src + i, 1, false))
            return -EFAULT;
        dst[i] = *(const char *)(src + i);
        if (dst[i] == '\0')
            return (long)i;
    }
    return -ENAMETOOLONG;
}

long copy_vector_from_user(uintptr_t uvec, char ***out)
{
    char **vec = kzalloc((USER_ARG_MAX + 1) * sizeof(char *));
    char *buf = kmalloc(USER_ARG_BYTES);
    if (!vec || !buf) {
        kfree(vec);
        kfree(buf);
        return -ENOMEM;
    }
    size_t used = 0, n = 0;
    long r = 0;
    for (;;) {
        if (!user_range_ok(uvec + n * sizeof(uintptr_t), sizeof(uintptr_t), false)) {
            r = -EFAULT;
            break;
        }
        uintptr_t s = *(const uintptr_t *)(uvec + n * sizeof(uintptr_t));
        if (!s)
            break;
        if (n == USER_ARG_MAX) {
            r = -E2BIG;
            break;
        }
        long len = copy_string_from_user(buf + used, s, USER_ARG_BYTES - used);
        if (len < 0) {
            r = len == -ENAMETOOLONG ? -E2BIG : len;
            break;
        }
        vec[n++] = buf + used;
        used += (size_t)len + 1;
    }
    if (r < 0) {
        kfree(vec);
        kfree(buf);
        return r;
    }
    vec[n] = NULL;
    /* Keep the buffer reachable for free_user_vector through slot MAX. */
    vec[USER_ARG_MAX] = buf;
    *out = vec;
    return (long)n;
}

void free_user_vector(char **vec)
{
    if (!vec)
        return;
    kfree(vec[USER_ARG_MAX]);
    kfree(vec);
}

void syscall_dispatch(struct trapframe *tf)
{
    arch_syscall_enter(tf);
    struct cpu *c = cpu_current();
    if (c->current)
        c->current->kentry_frame = tf;
    /* The entry masked interrupts. The kernel runs with them enabled. */
    arch_irq_enable();
    uint64_t nr = frame_syscall_nr(tf);
    long ret;
    if (nr < SYS_MAX && syscall_table[nr])
        ret = syscall_table[nr](tf);
    else
        ret = -ENOSYS;
    frame_set_retval(tf, (uint64_t)ret);
    proc_exit_check();
    signal_deliver(tf);
    arch_irq_disable();
    sched_preempt();
    if (nr == SYS_sigreturn)
        arch_syscall_return_full(tf);
}
