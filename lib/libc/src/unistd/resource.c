#include <sys/resource.h>
#include <minios/syscall.h>

int getrlimit(int resource, struct rlimit *rlim)
{
    return (int)syscall2(SYS_getrlimit, resource, rlim);
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    return (int)syscall2(SYS_setrlimit, resource, rlim);
}

int prlimit(pid_t pid, int resource, const struct rlimit *new_limit, struct rlimit *old_limit)
{
    return (int)syscall4(SYS_prlimit, pid, resource, new_limit, old_limit);
}

int getrusage(int who, struct rusage *usage)
{
    return (int)syscall2(SYS_getrusage, who, usage);
}
