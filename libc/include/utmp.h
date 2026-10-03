#pragma once
#include <sys/types.h>

/* The login records of BSD, for programs that name them. minios keeps
 * no utmp file, because login and the greeter tell init the session user
 * instead (docs/design/users.md). */
#define _PATH_UTMP "/var/run/utmp"
#define _PATH_WTMP "/var/log/wtmp"

#define UT_LINESIZE 32
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 64

#define EMPTY         0
#define LOGIN_PROCESS 6
#define USER_PROCESS  7
#define DEAD_PROCESS  8

struct utmp {
    char ut_line[UT_LINESIZE];
    char ut_name[UT_NAMESIZE];
    char ut_host[UT_HOSTSIZE];
    time_t ut_time;
    short ut_type;
    pid_t ut_pid;
};
