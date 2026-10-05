#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sched/wait.h>
#include <sync/mpsc.h>
#include <arch/thread.h>

#define THREAD_NAME_LEN 32
#define MLFQ_LEVELS 8

enum thread_state {
    THREAD_NEW,         /* allocated, not yet published to a run queue */
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

/* Scheduling fields are owned by the thread's per-CPU run queue.  State is
 * acquire/release published because remote wake producers inspect it before
 * placing wake_node on that CPU's MPSC inbox. */
struct thread {
    uint64_t *ctx;                  /* saved stack pointer while switched out */
    void *kstack_top;
    struct proc *proc;
    int tid;
    enum thread_state state;
    int level;                      /* MLFQ queue, 0 is highest priority */
    unsigned cpu;                   /* CPU whose run queue contains or last ran the thread */
    int slice_left;                 /* ms left in the current slice */
    uint64_t wake_at;               /* tick to wake a sleeping thread */
    unsigned sleep_cpu;             /* CPU whose sleeper list contains the sleeping thread, written under that run-queue lock */
    struct list_head run_link;      /* run queue, sleep list or wait queue */
    struct mpsc_node wake_node;     /* remote runnable notification */
    bool wake_queued;               /* atomically claims wake_node */
    struct waitq *waiting_on;       /* wait queue that contains run_link, wq->lock */
    bool sig_wake;                  /* a signal or exit was sent, atomic, see waitq_signal */
    uint32_t waitq_pins;            /* waitq_interrupt calls that use waiting_on, atomic */
    uint64_t bounded_since;         /* timer_ms + 1 when a bounded wait began, 0 otherwise, atomic */
    uint64_t hung_reported;         /* bounded_since of the last reported wait, written by hungd */
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
    struct arch_thread arch;        /* FPU state and TLS base (M23, M35, A0) */
    uint64_t utime, stime;          /* timer ticks charged to this thread, written by its CPU's tick (M40) */
    uint64_t nvcsw, nivcsw;         /* voluntary and involuntary switches away */
    /* Profiler timestamps in nanoseconds (M48). on_cpu_ns and off_cpu_ns
     * are written by the CPU running the switch, under its run queue lock;
     * ready_ns by whichever CPU makes the thread runnable. They feed
     * scheduler statistics only, so a race skews one number and nothing
     * else. All three are zero while the profiler is stopped. */
    uint64_t on_cpu_ns, off_cpu_ns, ready_ns;
    /* The trapframe of this thread's innermost entry from user mode,
     * written by the thread itself at that entry and read by the
     * profiler's unwinder, which validates it against the kernel stack
     * before following it. */
    struct trapframe *kentry_frame;
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
