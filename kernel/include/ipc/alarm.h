#pragma once
#include <kernel.h>

struct proc;

/* Send SIGALRM to the processes whose alarm is due. Called by the timer
 * interrupt on the boot processor. */
void alarm_tick(void);
/* Arm the alarm of p to fire after seconds, or cancel it with 0, and
 * return the seconds that were left of the previous one (alarm(2)). */
unsigned alarm_set(struct proc *p, unsigned seconds);
/* Cancel the alarm of a process that goes away. */
void alarm_cancel(struct proc *p);
