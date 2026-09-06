#pragma once
#include <sys/types.h>
#include <minios/abi.h>

/* Permission bits, stored by the filesystems and ignored by the kernel. */
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define ACCESSPERMS (S_IRWXU | S_IRWXG | S_IRWXO)
#define ALLPERMS (S_ISUID | S_ISGID | S_ISVTX | ACCESSPERMS)
#define DEFFILEMODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#ifndef S_IFLNK
#define S_IFLNK 0120000
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0140000
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#endif

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
/* There are no symbolic links, so lstat is stat. */
int lstat(const char *path, struct stat *st);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);
/* Device and FIFO nodes cannot be created: EPERM. */
int mknod(const char *path, mode_t mode, dev_t dev);
int mkfifo(const char *path, mode_t mode);
int mkdir(const char *path, mode_t mode);

/* Permission bits are stored but not enforced and cannot be changed: both
 * return 0 without effect. */
int fchmod(int fd, mode_t mode);
int chmod(const char *path, mode_t mode);

/* Set the modification time of path: times NULL for now, otherwise the
 * second timespec, with UTIME_NOW or UTIME_OMIT in tv_nsec. The path is
 * resolved as in openat. */
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
