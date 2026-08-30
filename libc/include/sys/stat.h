#pragma once
#include <sys/types.h>
#include <minios/abi.h>

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
