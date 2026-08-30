#pragma once
#include <sys/types.h>
#include <minios/abi.h>

typedef struct DIR DIR;

DIR *opendir(const char *path);
DIR *fdopendir(int fd);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
int dirfd(DIR *d);
/* Raw system call: fills whole struct dirent records, returns bytes. */
long getdents(int fd, struct dirent *buf, size_t count);
