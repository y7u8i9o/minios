#define KLOG_SUBSYS "hung"
#include <debug/hung.h>
#include <sched/proc.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <drivers/timer.h>
#include <lib/cmdline.h>
#include <mm/slab.h>
#include <console.h>
#include <lib/string.h>
#include <klog.h>

/* The hung task detector. A wait that ends soon in a working system, a
 * sleeping lock or a block request, calls waitq_wait_bounded, which
 * records when it began. hungd checks every thread once a second
 * and prints the thread table (proc_format_threads) on the console once
 * for every bounded wait that has lasted longer than the limit, so the
 * report shows the waiting thread and the threads it may wait for.
 * Alt+SysRq asks hungd for the same table at any time. */

#define HUNG_DEFAULT_SECONDS 30
#define HUNG_TABLE_SIZE 65536
#define HUNG_MAX_REPORTS 8

static unsigned limit_ms;               /* written before hungd starts */
static bool dump_requested;             /* atomic */

struct hung_scan {
    uint64_t now;
    unsigned n;
    struct {
        int pid, tid;
        uint64_t waited;
        char name[16];
    } found[HUNG_MAX_REPORTS];
};

void hung_request_dump(void)
{
    __atomic_store_n(&dump_requested, true, __ATOMIC_RELEASE);
}

static void print_table(void)
{
    char *text = kmalloc(HUNG_TABLE_SIZE);
    if (!text) {
        kprintf("hung: no memory for the thread table\n");
        return;
    }
    size_t len = proc_format_threads(text, HUNG_TABLE_SIZE);
    /* Line by line, so that the console output is not one long write. */
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            text[i] = '\0';
            kprintf("%s\n", text + start);
            start = i + 1;
        }
    }
    kfree(text);
}

/* Runs under proc_tree_lock and the process's lock. */
static void check_thread(struct proc *p, struct thread *t, void *arg)
{
    struct hung_scan *s = arg;
    uint64_t since = __atomic_load_n(&t->bounded_since, __ATOMIC_RELAXED);
    if (!since || since == t->hung_reported || s->now + 1 - since < limit_ms)
        return;
    t->hung_reported = since;
    if (s->n == HUNG_MAX_REPORTS)
        return;
    s->found[s->n].pid = p->pid;
    s->found[s->n].tid = t->tid;
    s->found[s->n].waited = s->now + 1 - since;
    strlcpy(s->found[s->n].name, p->name, sizeof s->found[s->n].name);
    s->n++;
}

static void hungd(void *arg)
{
    for (;;) {
        sleep_ms(1000);
        if (__atomic_exchange_n(&dump_requested, false, __ATOMIC_ACQ_REL)) {
            kprintf("hung: threads (Alt+SysRq)\n");
            print_table();
        }
        if (!limit_ms)
            continue;
        struct hung_scan scan = { .now = timer_ms() };
        proc_for_each_thread(check_thread, &scan);
        for (unsigned i = 0; i < scan.n; i++)
            kprintf("hung: pid %d tid %d (%s) in a bounded wait for %lu s\n", scan.found[i].pid,
                    scan.found[i].tid, scan.found[i].name, scan.found[i].waited / 1000);
        if (scan.n)
            print_table();
    }
}

void hung_start_daemon(void)
{
    char val[16];
    unsigned seconds = HUNG_DEFAULT_SECONDS;
    if (cmdline_lookup("hung_task", val, sizeof val)) {
        seconds = 0;
        for (const char *c = val; *c >= '0' && *c <= '9' && seconds < 100000; c++)
            seconds = seconds * 10 + (unsigned)(*c - '0');
    }
    limit_ms = seconds * 1000;
    if (!thread_create("hungd", hungd, NULL, 0))
        klog_warn("cannot start hungd");
}
