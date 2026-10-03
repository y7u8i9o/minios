#pragma once
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

extern char **environ;

ssize_t write(int fd, const void *buf, size_t n);
ssize_t read(int fd, void *buf, size_t n);
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
__attribute__((noreturn)) void _exit(int status);
pid_t getpid(void);
pid_t getppid(void);
void *sbrk(intptr_t increment);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int sched_yield(void);
/* The kernel id of the calling thread (M35). */
int gettid(void);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int dup(int fd);
int dup2(int fd, int fd2);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int ftruncate(int fd, long size);
int unlink(const char *path);
int rmdir(const char *path);
int link(const char *oldpath, const char *newpath);
int isatty(int fd);
void sync(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
int tcsetpgrp(int fd, pid_t pgrp);
pid_t tcgetpgrp(int fd);
unsigned sleep(unsigned seconds);
int usleep(unsigned long usec);
int sleep_ms(unsigned long ms);
/* Milliseconds since boot. */
long uptime_ms(void);
/* Number of processors and the processor currently running the caller. */
int nproc(void);
int getcpu(void);

extern char *optarg;
extern int optind, opterr, optopt, optreset;
int getopt(int argc, char *const argv[], const char *optstring);

/* Ownership is stored but not enforced and cannot be changed: both return 0
 * without effect. */
int fchown(int fd, uid_t owner, gid_t group);
int chown(const char *path, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);

/* Real, effective and saved ids of the process (docs/design/users.md).
 * An unprivileged process may set each id only to one of its current
 * three, root to any value. setuid and setgid set all three for root and
 * only the effective id otherwise. The value (uid_t)-1 leaves an id of the
 * res calls unchanged. */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t uid);
int setegid(gid_t gid);
int setreuid(uid_t ruid, uid_t euid);
int setregid(gid_t rgid, gid_t egid);
int setresuid(uid_t ruid, uid_t euid, uid_t suid);
int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
/* Supplementary groups, at most NGROUPS_MAX. Setting them requires root. */
int getgroups(int size, gid_t list[]);
int setgroups(size_t size, const gid_t *list);
/* The account of the session (LOGNAME, else the real uid). */
char *getlogin(void);
/* SHA-256 crypt ("$5$"), the only method: NULL with EINVAL otherwise. */
char *crypt(const char *key, const char *salt);

/* symlink creates path as a symbolic link holding target, which is not
 * checked and may name nothing. readlink copies at most size bytes of a
 * link's target into buf without a terminating NUL and returns the count.
 * The *at forms resolve a relative path from the directory dirfd. */
int symlink(const char *target, const char *path);
int symlinkat(const char *target, int dirfd, const char *path);
ssize_t readlink(const char *path, char *buf, size_t size);
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t size);

int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);

/* Scatter and gather I/O, performed as a sequence of read and write calls. */
struct iovec;
ssize_t readv(int fd, const struct iovec *iov, int count);
ssize_t writev(int fd, const struct iovec *iov, int count);

/* access checks that the file exists; permission bits are not enforced. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char *path, int mode);

/* confstr knows _CS_PATH, the default command search path "/bin". */
#define _CS_PATH 0
size_t confstr(int name, char *buf, size_t len);
