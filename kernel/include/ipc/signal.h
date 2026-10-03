#pragma once
#include <kernel.h>
#include <minios/abi.h>

struct proc;
struct thread;
struct trapframe;

/* Kernel copy of a disposition. Protected by proc.lock. */
struct ksigaction {
    void (*handler)(int);
    uint64_t mask;
    int flags;
    void (*restorer)(void);
};

/* signal_send_from posts sig to p as sent by the process from with kill
 * (si_code SI_USER and its pid and real uid), signal_send as sent by the
 * kernel (SI_KERNEL). */
struct proc;
int signal_send_from(struct proc *p, int sig, struct proc *from);
/* Post sig to p. SIGKILL terminates immediately; ignored signals are
 * dropped; others become pending and interrupt blocked threads. */
int signal_send(struct proc *p, int sig);
/* Send to every process of group pgid. Returns the number reached. */
int signal_send_pgrp(int pgid, int sig);
/* True if the current thread must leave a blocking operation: the process
 * is exiting or an unmasked signal is pending. */
bool signal_should_interrupt(void);
/* Deliver one pending signal before returning to user mode with tf. */
void signal_deliver(struct trapframe *tf);
/* Reset dispositions for exec: handlers become default, ignores remain. */
void signal_reset_for_exec(struct proc *p);
/* Copy dispositions from parent to child (fork). */
void signal_copy(struct proc *dst, const struct proc *src);
/* Deliver a synchronous fault signal: returns true if the process has a
 * handler and the signal was queued, false if the default applies. */
bool signal_fault(struct proc *p, int sig);
/* Called by sigreturn. */
long signal_return(struct trapframe *tf);
