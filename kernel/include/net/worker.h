#pragma once
/* The networking worker (N02): one kernel thread, netd, that runs every
 * protocol state transition, every deadline and every request from a
 * system call. It never blocks on a peer, on application buffer space or
 * on a device descriptor: it dequeues, runs a bounded batch, checks the
 * timers and sleeps until work arrives or the earliest deadline, taken
 * from the network clock, expires. Producers lock net_worker.lock while
 * they enqueue and wake, and the worker checks its queues under the same
 * lock before sleeping, so nothing that arrives just before the sleep
 * is lost. */
#include <kernel.h>
#include <lib/list.h>
#include <net/pbuf.h>

#define NET_INPUT_QUEUE_MAX 128     /* packets waiting for the worker */
#define NET_REQUEST_MAX     64      /* requests waiting for the worker */
#define NET_SERVICE_MAX     4       /* driver service functions */
#define NET_BATCH           32      /* packets, then requests, between timer checks */

/* A request: fn runs on the worker with no lock acquired and its return
 * value becomes result. The requester either waits with
 * net_request_wait, which owns the storage until the request is done,
 * or sets done, which the worker calls last and which may free the
 * request; not both. link and state are protected by net_worker.lock. */
struct net_request {
    struct list_head link;
    int (*fn)(struct net_request *r);
    void (*done)(struct net_request *r);
    int result;
    int state;
};

enum {
    NET_REQ_IDLE,
    NET_REQ_QUEUED,
    NET_REQ_RUNNING,
    NET_REQ_DONE,
    NET_REQ_CANCELLED,
};

void net_request_init(struct net_request *r, int (*fn)(struct net_request *));
/* Queue r; -ENOBUFS when NET_REQUEST_MAX requests are already waiting. */
int net_request_submit(struct net_request *r);
/* Wait for r to finish and return its result. A signal or process exit
 * while r is still queued withdraws it and returns -EINTR; a running
 * request is always waited for, which is bounded because the worker
 * does not block. */
int net_request_wait(struct net_request *r);
/* Uninterruptible completion for teardown and lifetime barriers. */
int net_request_finish(struct net_request *r);
/* Withdraw a queued request; false once the worker has taken it. */
bool net_request_cancel(struct net_request *r);
/* submit followed by wait. */
int net_request_run(struct net_request *r);

/* A one shot deadline on the network clock; fn runs on the worker with
 * no lock acquired and may re-arm. link, deadline and armed are protected
 * by net_worker.lock. */
struct net_timer {
    struct list_head link;
    uint64_t deadline_ms;
    void (*fn)(struct net_timer *t);
    bool armed;
};

void net_timer_init(struct net_timer *t, void (*fn)(struct net_timer *));
/* Arm or move t to deadline_ms; a deadline already reached fires at the
 * worker's next pass. */
void net_timer_arm(struct net_timer *t, uint64_t deadline_ms);
/* Disarm t; false when it was not armed, which includes a timer whose
 * fn is running or has run. A caller that must be sure fn is not running
 * synchronizes with a request. */
bool net_timer_cancel(struct net_timer *t);
bool net_timer_armed(struct net_timer *t);

/* Queue an input packet (owner stack -> queue); -ENOBUFS when full. */
int net_worker_queue_packet(struct pbuf *p);
/* Wake the worker so it re-evaluates its deadlines: used after the
 * controlled clock moved. */
void net_worker_kick(void);
/* Run an empty request: on return every packet and request queued
 * before the call has been processed. */
void net_worker_drain(void);
bool net_worker_is_current(void);
void net_worker_start(void);
/* Register a driver's service function. The worker calls each registered
 * function once per pass, after running timers and before processing
 * input packets; drivers use it to reclaim transmits and collect received
 * frames. Registrations cannot be removed. Returns -ENOSPC once
 * NET_SERVICE_MAX functions are registered. */
int net_worker_add_service(void (*fn)(void));

struct net_worker_stats {
    uint64_t packets;               /* input packets processed */
    uint64_t packets_dropped_full;  /* refused because the queue was full */
    uint64_t packets_dropped_down;  /* dequeued for an interface that went down */
    uint64_t requests;              /* requests run */
    uint64_t requests_rejected;     /* refused because the queue was full */
    uint64_t requests_cancelled;
    uint64_t timers_fired;
    uint64_t batches;
    uint64_t sleeps;
    uint32_t queued_packets;
    uint32_t queued_requests;
    uint32_t armed_timers;
};
void net_worker_get_stats(struct net_worker_stats *out);
