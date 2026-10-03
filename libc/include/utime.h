#pragma once
#include <sys/types.h>

/* utime sets the modification time of path to times->modtime, or to the
 * current time when times is NULL. minios stores no access time. */
struct utimbuf {
    time_t actime;
    time_t modtime;
};

int utime(const char *path, const struct utimbuf *times);
