#pragma once
#include <sys/types.h>
#include <minios/abi.h>

/* Permission bits, checked by the kernel against the credentials of the
 * caller (docs/design/users.md). S_ISUID, S_ISGID and S_ISVTX come from
 * minios/abi.h. */
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
#define ACCESSPERMS (S_IRWXU | S_IRWXG | S_IRWXO)
#define ALLPERMS (S_ISUID | S_ISGID | S_ISVTX | ACCESSPERMS)
#define DEFFILEMODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#ifndef S_IFSOCK
#define S_IFSOCK 0140000
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#endif

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
/* lstat reports on a symbolic link itself, as fstatat does with
 * AT_SYMLINK_NOFOLLOW; st_size is the length of the link's target. */
int lstat(const char *path, struct stat *st);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);
/* Device and FIFO nodes cannot be created: EPERM. */
int mknod(const char *path, mode_t mode, dev_t dev);
int mkfifo(const char *path, mode_t mode);
int mkdir(const char *path, mode_t mode);

/* Change the permission bits. Only the owner and root may do so, and the
 * set group id bit is dropped when the caller is not in the file's group.
 * fchmodat takes AT_SYMLINK_NOFOLLOW, which changes a symbolic link
 * itself. */
int fchmod(int fd, mode_t mode);
int chmod(const char *path, mode_t mode);
int fchmodat(int dirfd, const char *path, mode_t mode, int flags);

/* Set the file creation mask of the process and return the old one. The
 * mask starts at 022. */
mode_t umask(mode_t mask);

/* Set the modification time of path: times NULL for now, otherwise the
 * second timespec, with UTIME_NOW or UTIME_OMIT in tv_nsec. The path is
 * resolved as in openat; with AT_SYMLINK_NOFOLLOW a symbolic link named
 * by path gets the time itself. */
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
