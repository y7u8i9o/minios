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

/* Scatter and gather I/O, performed as a sequence of read and write calls. */
struct iovec;
ssize_t readv(int fd, const struct iovec *iov, int count);
ssize_t writev(int fd, const struct iovec *iov, int count);
