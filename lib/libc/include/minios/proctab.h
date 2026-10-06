#pragma once
#include <sys/types.h>

/* The process table of /dev/proc, one row per process. The kernel writes
 * the columns PID PPID PGID STATE TIME RSS UID NAME
 * (kernel/sched/proc.c, proc_format_table). */
struct proc_entry {
    pid_t pid, ppid, pgid;
    char state[12];             /* running, stopped or zombie */
    unsigned long ticks;        /* CPU time in ticks of one millisecond */
    unsigned long rss_kib;      /* resident pages in KiB, shared pages included */
    unsigned uid;               /* effective uid */
    char name[32];
};

/* Reads up to max rows into rows. Returns the number of rows, or a
 * negative errno value when /dev/proc cannot be read. */
int proc_table_read(struct proc_entry *rows, int max);
/* The row of pid. Returns 0, -ESRCH when no row has the pid, or another
 * negative errno value when /dev/proc cannot be read. */
int proc_table_find(pid_t pid, struct proc_entry *out);
