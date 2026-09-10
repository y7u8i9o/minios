/* netd, the networking worker. net_worker.lock protects the packet and
 * request queues, the timer list, the kick flag and the statistics; it
 * is the condition lock of waitq (the worker's sleep) and done_waitq
 * (request completion), so it sits with the other condition locks above
 * waitq.lock and timed_lock. Nothing is held while a packet, a request
 * or a timer function runs. */
#define KLOG_SUBSYS "netd"
#include <net/worker.h>
#include <net/netif.h>
#include <net/net.h>
#include <net/clock.h>
#include <sched/wait.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <ipc/signal.h>
#include <sync/spinlock.h>
#include <debug/panic.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

struct net_worker {
    struct spinlock lock;
    struct list_head packets;
    uint32_t npackets;
    struct list_head requests;
    uint32_t nrequests;
    struct list_head timers;        /* armed, earliest deadline first */
    uint32_t ntimers;
    bool kicked;
    struct waitq waitq;
    struct waitq done_waitq;
    struct thread *thread;
    struct net_worker_stats stats;
};

static struct net_worker w = {
    .lock = SPINLOCK_INIT("net_worker"),
    .packets = LIST_HEAD_INIT(w.packets),
    .requests = LIST_HEAD_INIT(w.requests),
    .timers = LIST_HEAD_INIT(w.timers),
    .waitq = WAITQ_INIT(w.waitq),
    .done_waitq = WAITQ_INIT(w.done_waitq),
};

static void wake_locked(void)
{
    w.kicked = true;
    waitq_wake_all(&w.waitq);
}

/* ---- requests ---- */

void net_request_init(struct net_request *r, int (*fn)(struct net_request *))
{
    r->fn = fn;
    r->done = NULL;
    r->result = 0;
    r->state = NET_REQ_IDLE;
}

int net_request_submit(struct net_request *r)
{
    spin_lock(&w.lock);
    if (w.nrequests >= NET_REQUEST_MAX) {
        w.stats.requests_rejected++;
        spin_unlock(&w.lock);
        return -ENOBUFS;
    }
    kassert(r->state != NET_REQ_QUEUED && r->state != NET_REQ_RUNNING);
    r->state = NET_REQ_QUEUED;
    list_add_tail(&r->link, &w.requests);
    w.nrequests++;
    wake_locked();
    spin_unlock(&w.lock);
    return 0;
}

int net_request_wait(struct net_request *r)
{
    spin_lock(&w.lock);
    while (r->state != NET_REQ_DONE) {
        if (r->state == NET_REQ_QUEUED && signal_should_interrupt()) {
            list_del(&r->link);
            w.nrequests--;
            r->state = NET_REQ_CANCELLED;
            w.stats.requests_cancelled++;
            spin_unlock(&w.lock);
            return -EINTR;
        }
        if (r->state == NET_REQ_CANCELLED) {
            spin_unlock(&w.lock);
            return -EINTR;
        }
        waitq_wait(&w.done_waitq, &w.lock);
    }
    spin_unlock(&w.lock);
    return r->result;
}

bool net_request_cancel(struct net_request *r)
{
    spin_lock(&w.lock);
    bool cancelled = r->state == NET_REQ_QUEUED;
    if (cancelled) {
        list_del(&r->link);
        w.nrequests--;
        r->state = NET_REQ_CANCELLED;
        w.stats.requests_cancelled++;
    }
    spin_unlock(&w.lock);
    return cancelled;
}

int net_request_run(struct net_request *r)
{
    int e = net_request_submit(r);
    return e < 0 ? e : net_request_wait(r);
}

static int drain_fn(struct net_request *r)
{
    return 0;
}

void net_worker_drain(void)
{
    struct net_request r;
    net_request_init(&r, drain_fn);
    /* A full queue is waited out: the drain is a synchronization point,
     * not a hint. */
    while (net_request_submit(&r) == -ENOBUFS)
        sched_yield();
    net_request_wait(&r);
}

/* ---- timers ---- */

void net_timer_init(struct net_timer *t, void (*fn)(struct net_timer *))
{
    t->fn = fn;
    t->armed = false;
    t->deadline_ms = 0;
}

void net_timer_arm(struct net_timer *t, uint64_t deadline_ms)
{
    spin_lock(&w.lock);
    if (t->armed) {
        list_del(&t->link);
        w.ntimers--;
    }
    t->deadline_ms = deadline_ms;
    t->armed = true;
    struct list_head *pos;
    list_for_each(pos, &w.timers) {
        struct net_timer *o = list_entry(pos, struct net_timer, link);
        if (deadline_ms < o->deadline_ms)
            break;
    }
    list_insert_between(&t->link, pos->prev, pos);
    w.ntimers++;
    /* The worker recomputes its sleep from the head of the list. */
    if (w.timers.next == &t->link)
        wake_locked();
    spin_unlock(&w.lock);
}

bool net_timer_cancel(struct net_timer *t)
{
    spin_lock(&w.lock);
    bool was = t->armed;
    if (was) {
        list_del(&t->link);
        w.ntimers--;
        t->armed = false;
    }
    spin_unlock(&w.lock);
    return was;
}

bool net_timer_armed(struct net_timer *t)
{
    spin_lock(&w.lock);
    bool armed = t->armed;
    spin_unlock(&w.lock);
    return armed;
}

/* Fire every timer whose deadline has been reached, one at a time with
 * the lock released around fn, which may arm or cancel timers. */
static void run_timers(void)
{
    for (;;) {
        spin_lock(&w.lock);
        if (list_empty(&w.timers)) {
            spin_unlock(&w.lock);
            return;
        }
        struct net_timer *t = list_first_entry(&w.timers, struct net_timer, link);
        if (t->deadline_ms > net_clock_ms()) {
            spin_unlock(&w.lock);
            return;
        }
        list_del(&t->link);
        w.ntimers--;
        t->armed = false;
        w.stats.timers_fired++;
        spin_unlock(&w.lock);
        t->fn(t);
    }
}

/* ---- packets ---- */

int net_worker_queue_packet(struct pbuf *p)
{
    int r = pbuf_transfer(p, PBUF_OWNER_STACK, PBUF_OWNER_QUEUE);
    if (r < 0)
        return r;
    spin_lock(&w.lock);
    if (w.npackets >= NET_INPUT_QUEUE_MAX) {
        w.stats.packets_dropped_full++;
        spin_unlock(&w.lock);
        pbuf_transfer(p, PBUF_OWNER_QUEUE, PBUF_OWNER_STACK);
        return -ENOBUFS;
    }
    list_add_tail(&p->link, &w.packets);
    w.npackets++;
    wake_locked();
    spin_unlock(&w.lock);
    return 0;
}

static void run_packets(void)
{
    for (int i = 0; i < NET_BATCH; i++) {
        spin_lock(&w.lock);
        if (list_empty(&w.packets)) {
            spin_unlock(&w.lock);
            return;
        }
        struct pbuf *p = list_first_entry(&w.packets, struct pbuf, link);
        list_del(&p->link);
        w.npackets--;
        w.stats.packets++;
        spin_unlock(&w.lock);
        int r = pbuf_transfer(p, PBUF_OWNER_QUEUE, PBUF_OWNER_STACK);
        kassert(r == 0);
        struct netif *n = p->netif;
        if (!n || !netif_is_up(n)) {
            spin_lock(&w.lock);
            w.stats.packets_dropped_down++;
            spin_unlock(&w.lock);
            if (n)
                atomic_u64_fetch_add_relaxed(&n->stats.rx_dropped, 1);
            pbuf_free(p);
            continue;
        }
        if (n->ops->input)
            n->ops->input(n, p);
        else
            net_ip_input(n, p);
    }
}

static void run_requests(void)
{
    for (int i = 0; i < NET_BATCH; i++) {
        spin_lock(&w.lock);
        if (list_empty(&w.requests)) {
            spin_unlock(&w.lock);
            return;
        }
        struct net_request *r = list_first_entry(&w.requests, struct net_request, link);
        list_del(&r->link);
        w.nrequests--;
        r->state = NET_REQ_RUNNING;
        w.stats.requests++;
        spin_unlock(&w.lock);
        int result = r->fn(r);
        void (*done)(struct net_request *) = r->done;
        spin_lock(&w.lock);
        r->result = result;
        r->state = NET_REQ_DONE;
        waitq_wake_all(&w.done_waitq);
        spin_unlock(&w.lock);
        /* Neither the waiter nor done may be reached after this point
         * through r: a waiter frees it once it sees DONE. */
        if (done)
            done(r);
    }
}

/* ---- the thread ---- */

void net_worker_kick(void)
{
    spin_lock(&w.lock);
    wake_locked();
    spin_unlock(&w.lock);
}

bool net_worker_is_current(void)
{
    return thread_current() == w.thread;
}

static void netd(void *arg)
{
    for (;;) {
        spin_lock(&w.lock);
        for (;;) {
            bool due = false;
            if (!list_empty(&w.timers)) {
                struct net_timer *t = list_first_entry(&w.timers, struct net_timer, link);
                due = t->deadline_ms <= net_clock_ms();
            }
            if (w.kicked || due || !list_empty(&w.packets) || !list_empty(&w.requests))
                break;
            w.stats.sleeps++;
            /* With the controlled clock a deadline is reached only when
             * the test moves time and kicks, so the sleep has no limit. */
            if (!list_empty(&w.timers) && !net_clock_is_controlled()) {
                struct net_timer *t = list_first_entry(&w.timers, struct net_timer, link);
                waitq_wait_timeout(&w.waitq, &w.lock, t->deadline_ms);
            } else {
                waitq_wait(&w.waitq, &w.lock);
            }
        }
        w.kicked = false;
        w.stats.batches++;
        spin_unlock(&w.lock);
        run_timers();
        run_packets();
        run_requests();
    }
}

void net_worker_start(void)
{
    w.thread = thread_create("netd", netd, NULL, 0);
    if (!w.thread)
        panic("cannot create netd");
}

void net_worker_get_stats(struct net_worker_stats *out)
{
    spin_lock(&w.lock);
    *out = w.stats;
    out->queued_packets = w.npackets;
    out->queued_requests = w.nrequests;
    out->armed_timers = w.ntimers;
    spin_unlock(&w.lock);
}
