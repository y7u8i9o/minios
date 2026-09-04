#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sched/wait.h>

#define THREAD_NAME_LEN 32
#define MLFQ_LEVELS 8

enum thread_state {
    THREAD_READY,       /* on a run queue */
    THREAD_RUNNING,
    THREAD_BLOCKED,     /* on a wait queue */
    THREAD_SLEEPING,    /* on the sleep list, woken by the timer */
    THREAD_STOPPED,     /* process stopped by job-control signal */
    THREAD_ZOMBIE,      /* finished, waiting to be joined */
};

struct proc;
struct trapframe;

typedef void (*thread_fn)(void *arg);

/* A schedulable thread. state, level, slice_left, wake_at and run_link are
 * protected by sched_lock. The remaining fields are set at creation or
 * only touched by the thread itself. */
struct thread {
    uint64_t *ctx;                  /* saved stack pointer while switched out */
    void *kstack_top;
    struct proc *proc;
    int tid;
    enum thread_state state;
    int level;                      /* MLFQ queue, 0 is highest priority */
    unsigned cpu;                   /* CPU whose run queue holds or last ran the thread */
    int slice_left;                 /* ms left in the current slice */
    uint64_t wake_at;               /* tick to wake a sleeping thread */
    struct list_head run_link;      /* run queue, sleep list or wait queue */
    struct waitq *waiting_on;       /* wait queue holding run_link, wq->lock */
    struct list_head proc_link;     /* proc->threads, proc->lock */
    thread_fn entry;
    void *arg;
    int exit_code;
    struct spinlock exit_lock;      /* protects finished */
    bool finished;                  /* stack no longer in use, joinable */
    struct waitq exit_waitq;        /* joiners, condition lock is exit_lock */
    struct trapframe *user_frame;   /* entry frame for user threads (M8) */
    bool on_boot_stack;             /* stack not owned by kstack_alloc */
    uint64_t sig_mask;              /* blocked signals, used by the thread itself */
    void *fpu;                      /* fxsave area, 16 byte aligned inside fpu_raw (M23) */
    void *fpu_raw;
    uint64_t fs_base;               /* user FS base (thread local storage), loaded at every switch (M35) */
    uint64_t utime, stime;          /* timer ticks charged to this thread, written by its CPU's tick (M40) */
    uint64_t nvcsw, nivcsw;         /* voluntary and involuntary switches away, sched_lock */
    void *fs_txn;                   /* filesystem transaction the thread is inside, if any (M36) */
    int fs_txn_depth;               /* nesting of op_begin calls for fs_txn */
    char name[THREAD_NAME_LEN];
};

/* Create a thread in the kernel process at the given MLFQ level and make
 * it runnable. Returns NULL on allocation failure. */
struct thread *thread_create(const char *name, thread_fn fn, void *arg, int level);
/* Same, but the thread is not enqueued and belongs to proc. Used by the
 * process layer, which sets up the entry frame before sched_wake. */
struct thread *thread_alloc(struct proc *proc, const char *name, thread_fn fn, void *arg, int level);
__noreturn void thread_exit(int code);
/* Wait for a thread to finish, then free it. Returns its exit code. */
int thread_join(struct thread *t);
struct thread *thread_current(void);
/* Free a zombie without joining. Used by the process layer. */
void thread_free(struct thread *t);
