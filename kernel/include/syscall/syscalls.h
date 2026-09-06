#pragma once
#include <kernel.h>

struct trapframe;

typedef long (*syscall_fn)(struct trapframe *tf);

/* Argument accessors for handlers. */
#define SYSARG0(tf) ((tf)->rdi)
#define SYSARG1(tf) ((tf)->rsi)
#define SYSARG2(tf) ((tf)->rdx)
#define SYSARG3(tf) ((tf)->r10)
#define SYSARG4(tf) ((tf)->r8)
#define SYSARG5(tf) ((tf)->r9)

#define USER_PATH_MAX  256
#define USER_ARG_MAX   64
#define USER_ARG_BYTES 4096

/* True if [addr, addr + len) lies inside mapped regions of the current
 * process with the required access. */
bool user_range_ok(uintptr_t addr, size_t len, bool write);
/* Copy a NUL terminated string from user space. Returns length or -errno. */
long copy_string_from_user(char *dst, uintptr_t src, size_t max);
/* Copy a NULL terminated pointer vector of strings. On success *out is a
 * kmalloc'd array whose strings live in one kmalloc'd buffer; free with
 * free_user_vector. */
long copy_vector_from_user(uintptr_t uvec, char ***out);
void free_user_vector(char **vec);

long sys_write(struct trapframe *tf);
long sys_read(struct trapframe *tf);
long sys_exit(struct trapframe *tf);
long sys_getpid(struct trapframe *tf);
long sys_getppid(struct trapframe *tf);
long sys_yield(struct trapframe *tf);
long sys_fork(struct trapframe *tf);
long sys_execve(struct trapframe *tf);
long sys_wait4(struct trapframe *tf);
long sys_kill(struct trapframe *tf);
long sys_thread_create(struct trapframe *tf);
long sys_thread_exit(struct trapframe *tf);
long sys_thread_join(struct trapframe *tf);
long sys_sbrk(struct trapframe *tf);
long sys_mprotect(struct trapframe *tf);
long sys_msync(struct trapframe *tf);
long sys_madvise(struct trapframe *tf);
long sys_getrlimit(struct trapframe *tf);
long sys_setrlimit(struct trapframe *tf);
long sys_prlimit(struct trapframe *tf);
long sys_getrusage(struct trapframe *tf);
long sys_chdir(struct trapframe *tf);
long sys_getcwd(struct trapframe *tf);
long sys_open(struct trapframe *tf);
long sys_close(struct trapframe *tf);
long sys_lseek(struct trapframe *tf);
long sys_dup(struct trapframe *tf);
long sys_dup2(struct trapframe *tf);
long sys_stat(struct trapframe *tf);
long sys_fstat(struct trapframe *tf);
long sys_getdents(struct trapframe *tf);
long sys_mkdir(struct trapframe *tf);
long sys_unlink(struct trapframe *tf);
long sys_rmdir(struct trapframe *tf);
long sys_rename(struct trapframe *tf);
long sys_link(struct trapframe *tf);
long sys_pipe(struct trapframe *tf);
long sys_mount(struct trapframe *tf);
long sys_umount(struct trapframe *tf);
long sys_sync(struct trapframe *tf);
long sys_reboot(struct trapframe *tf);
long sys_sigaction(struct trapframe *tf);
long sys_sigprocmask(struct trapframe *tf);
long sys_sigreturn(struct trapframe *tf);
long sys_setpgid(struct trapframe *tf);
long sys_getpgid(struct trapframe *tf);
long sys_tcsetpgrp(struct trapframe *tf);
long sys_tcgetpgrp(struct trapframe *tf);
long sys_ioctl(struct trapframe *tf);
long sys_sleep_ms(struct trapframe *tf);
long sys_set_tls(struct trapframe *tf);
long sys_gettid(struct trapframe *tf);
long sys_futex(struct trapframe *tf);
long sys_clock_gettime(struct trapframe *tf);
long sys_clock_settime(struct trapframe *tf);
long sys_mq_open(struct trapframe *tf);
long sys_mq_unlink(struct trapframe *tf);
long sys_shm_open(struct trapframe *tf);
long sys_shm_unlink(struct trapframe *tf);
long sys_poll(struct trapframe *tf);
long sys_uptime_ms(struct trapframe *tf);
long sys_nproc(struct trapframe *tf);
long sys_getcpu(struct trapframe *tf);
long sys_uname(struct trapframe *tf);
long sys_mmap(struct trapframe *tf);
long sys_munmap(struct trapframe *tf);
/* M23 */
long sys_socket(struct trapframe *tf);
long sys_socketpair(struct trapframe *tf);
long sys_bind(struct trapframe *tf);
long sys_listen(struct trapframe *tf);
long sys_accept(struct trapframe *tf);
long sys_connect(struct trapframe *tf);
long sys_sendmsg(struct trapframe *tf);
long sys_recvmsg(struct trapframe *tf);
long sys_shutdown(struct trapframe *tf);
long sys_memfd_create(struct trapframe *tf);
long sys_ftruncate(struct trapframe *tf);
long sys_eventfd(struct trapframe *tf);
long sys_timerfd_create(struct trapframe *tf);
long sys_timerfd_settime(struct trapframe *tf);
long sys_timerfd_gettime(struct trapframe *tf);
long sys_fcntl(struct trapframe *tf);
long sys_pipe2(struct trapframe *tf);
long sys_utimensat(struct trapframe *tf);
long sys_openat(struct trapframe *tf);
long sys_fstatat(struct trapframe *tf);
