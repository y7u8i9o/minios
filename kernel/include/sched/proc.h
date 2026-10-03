#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sync/spinlock.h>
#include <sched/wait.h>
#include <fs/fdtable.h>
#include <ipc/signal.h>
#include <sched/cred.h>

#define PROC_NAME_LEN 32
#define PROC_CWD_LEN  256

enum proc_state {
    PROC_RUNNING,
    PROC_ZOMBIE,
};

struct vmspace;
struct thread;

/* A process: an address space and a set of threads.
 * lock protects threads, zombies, nthreads, exit_status, cwd,
 * sig_actions and cred. exiting and sig_pending are atomic words. fds has its own lock. pgid and sid are
 * protected by proc_tree_lock.
 * parent, children, sibling and state are protected by proc_tree_lock. */
struct proc {
    int pid;
    struct vmspace *vm;             /* NULL for the kernel process */
    struct list_head threads;       /* live threads */
    struct list_head zombies;       /* exited threads awaiting join or reap */
    int nthreads;
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;       /* parent->children */
    enum proc_state state;
    bool stopped;                   /* protected by proc_tree_lock */
    bool stop_reported;             /* wait4 has reported this stop */
    bool continued;                 /* continuation waiting for wait4 */
    int stop_signal;
    bool exiting;                   /* atomic: threads exit at the next kernel exit */
    int exit_status;                /* wait4 status encoding */
    struct spinlock lock;
    struct list_head link;          /* proc_list */
    struct waitq exit_waitq;        /* woken when the process becomes a zombie */
    struct waitq child_waitq;       /* woken when a child becomes a zombie */
    char name[PROC_NAME_LEN];
    char cwd[PROC_CWD_LEN];
    struct fdtable fds;             /* open files */
    int pgid;                       /* process group */
    int sid;                        /* session, the pid of its leader (U5) */
    uint64_t sig_pending;           /* atomic pending-signal bitmap */
    struct ksigaction sig_actions[NSIG];
    /* The sender of each pending signal, for SA_SIGINFO (U5), under lock. */
    siginfo_t sig_info[NSIG];
    /* The deadline of alarm(2) in timer_ms and the link in the list of
     * armed processes (U5), both under alarm_lock. */
    struct list_head alarm_link;
    uint64_t alarm_ms;
    /* Resource limits (M40): written under lock, read without it by the
     * timer tick and the enforcement points, which tolerate a stale value. */
    struct rlimit rlim[RLIMIT_NLIMITS];
    /* Identity (U0): written under lock, read through cred_get. */
    struct cred cred;
    /* CPU accounting in timer ticks and event counters, atomic updates;
     * the c* fields sum reaped children and are protected by lock. */
    uint64_t utime, stime;
    uint64_t minflt, majflt;
    uint64_t nvcsw, nivcsw;
    uint64_t cutime, cstime, cminflt, cmajflt, cnvcsw, cnivcsw;
};

/* wait4 status encoding. */
#define PROC_STATUS_EXITED(code)   (((code) & 0xff) << 8)
#define PROC_STATUS_SIGNALED(sig)  ((sig) & 0x7f)
#define PROC_STATUS_STOPPED(sig)   ((((sig) & 0xff) << 8) | 0x7f)
#define PROC_STATUS_CONTINUED      0xffff

/* Process 0, owner of every kernel thread. */
extern struct proc kernel_proc;
/* Serializes the parent/child tree and state transitions. */
extern struct spinlock proc_tree_lock;

void proc_init(void);
struct proc *proc_alloc(const char *name, struct proc *parent);
void proc_free(struct proc *p);
struct proc *proc_find(int pid);
/* Called by the last thread of a process: marks it zombie, reparents its
 * children to init and wakes its parent. */
void proc_exit_notify(struct proc *p);
/* Begin process exit from any thread: record the status and make every
 * other thread exit at its next kernel exit. */
void proc_begin_exit(struct proc *p, int status);
/* Free a zombie's threads and address space, return its status. Waits for
 * the process to exit first. */
int proc_reap(struct proc *p);
/* Reap every child of parent, waiting for each to exit. The kernel tests
 * use it for the children that a program under test left behind, which
 * the kernel process adopts when no init runs. */
void proc_reap_children(struct proc *parent);
/* Number of live (non zombie) processes other than kernel and init. */
int proc_count_others(void);
/* Collect the pids of group pgid (pgid 0: every user process except init).
 * Returns the count stored, at most max. */
int proc_collect_pgrp(int pgid, int *pids, int max);
/* Format "PID PPID PGID STATE NAME" lines into buf. Returns the length. */
size_t proc_format_table(char *buf, size_t size);
/* Every thread of every process with its state, the wait queue it blocks
 * on and the kernel frames where it was switched out (/dev/threads). */
size_t proc_format_threads(char *buf, size_t size);
/* Format one "CPU USER SYSTEM IDLE" line of tick counts per CPU into buf
 * (/dev/cpustat). Returns the length. */
size_t proc_format_cpustat(char *buf, size_t size);
/* Call fn for every thread of every process, under proc_tree_lock and the
 * process's lock, so fn must not sleep or take those locks. */
struct thread;
void proc_for_each_thread(void (*fn)(struct proc *p, struct thread *t, void *arg), void *arg);
/* Format the file backed regions of every process for /dev/maps: one line
 * per region with the pid, the start and end addresses, the file offset of
 * the start and the file's path. */
size_t proc_format_maps(char *buf, size_t size);
/* Number of live user processes (everything but the kernel process). */
int proc_count_users(void);
/* Charge the running thread and its process with one timer tick, user or
 * system depending on the interrupted mode, and enforce RLIMIT_CPU. Called
 * from the timer interrupt of every CPU. */
struct trapframe;
void proc_account_tick(const struct trapframe *tf);
/* The soft limit of a resource of the current process. */
uint64_t proc_rlimit_cur(struct proc *p, int resource);
/* Check point on the way back to user mode. */
void proc_exit_check(void);
/* Registered by kinit once init exists, target of reparenting. */
void proc_set_init(struct proc *p);
