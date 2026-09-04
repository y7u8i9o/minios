#pragma once
/* Resource limits and usage (M40). struct rlimit, struct rusage and the
 * RLIMIT_*, RLIM_INFINITY and RUSAGE_* constants come from the ABI header. */
#include <sys/types.h>
#include <minios/abi.h>

typedef uint64_t rlim_t;

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
/* Set and/or get the limits of another process; pid 0 is the caller.
 * Either pointer may be NULL. */
int prlimit(pid_t pid, int resource, const struct rlimit *new_limit, struct rlimit *old_limit);
int getrusage(int who, struct rusage *usage);
