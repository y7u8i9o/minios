#pragma once
/* Resource limits and usage (M40). struct rlimit, struct rusage and the
 * RLIMIT_*, RLIM_INFINITY and RUSAGE_* constants come from the ABI header. */
#include <sys/types.h>
#include <minios/abi.h>

typedef uint64_t rlim_t;
#define RLIM_NLIMITS RLIMIT_NLIMITS

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
/* Set and/or get the limits of another process; pid 0 is the caller.
 * Either pointer may be NULL. */
int prlimit(pid_t pid, int resource, const struct rlimit *new_limit, struct rlimit *old_limit);
int getrusage(int who, struct rusage *usage);

/* Scheduling priorities. Because the scheduler of minios adjusts
 * priorities by itself, getpriority reports 0 and setpriority accepts
 * only 0. */
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);
