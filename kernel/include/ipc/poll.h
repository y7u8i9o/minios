#pragma once
#include <kernel.h>
#include <lib/list.h>
#include <sync/spinlock.h>
#include <sched/wait.h>
#include <minios/abi.h>

struct file;

/* Per-object poll source.  A poll call installs one entry on each source
 * before checking readiness.  Producers walk only their own source, so an
 * event cannot wake consumers polling unrelated objects. */
struct poll_source {
    struct spinlock lock;
    struct list_head waiters;
};

void poll_source_init(struct poll_source *source, const char *name);
void poll_source_notify(struct poll_source *source);
long poll_files(struct file **files, struct pollfd *pfds, size_t n, long timeout_ms);
