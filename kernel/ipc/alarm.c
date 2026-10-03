/* alarm(2) sends SIGALRM after a number of seconds (U5). alarm_lock
 * protects the list of armed processes and the alarm fields of every
 * process. The timer interrupt takes it, which is why holders disable
 * interrupts, as spin_lock does. It is a leaf, because the tick collects
 * the due pids under it and sends the signals after releasing it. */
#include <ipc/alarm.h>
#include <ipc/signal.h>
#include <sched/proc.h>
#include <drivers/timer.h>
#include <lib/list.h>

static LIST_HEAD(armed);
static DEFINE_SPINLOCK(alarm_lock);

void alarm_tick(void)
{
    if (list_empty(&armed))
        return;
    uint64_t now = timer_ms();
    int due[16], n = 0;
    spin_lock(&alarm_lock);
    struct list_head *pos, *next;
    for (pos = armed.next; pos != &armed && n < (int)(sizeof due / sizeof due[0]); pos = next) {
        next = pos->next;
        struct proc *p = list_entry(pos, struct proc, alarm_link);
        if (now < p->alarm_ms)
            continue;
        list_del(&p->alarm_link);
        list_init(&p->alarm_link);
        p->alarm_ms = 0;
        due[n++] = p->pid;
    }
    spin_unlock(&alarm_lock);
    for (int i = 0; i < n; i++) {
        struct proc *p = proc_find(due[i]);
        if (p)
            signal_send(p, SIGALRM);
    }
}

unsigned alarm_set(struct proc *p, unsigned seconds)
{
    uint64_t now = timer_ms();
    spin_lock(&alarm_lock);
    unsigned left = 0;
    if (p->alarm_ms)
        left = p->alarm_ms > now ? (unsigned)((p->alarm_ms - now + 999) / 1000) : 1;
    if (!list_empty(&p->alarm_link)) {
        list_del(&p->alarm_link);
        list_init(&p->alarm_link);
    }
    p->alarm_ms = 0;
    if (seconds) {
        p->alarm_ms = now + (uint64_t)seconds * 1000;
        list_add_tail(&p->alarm_link, &armed);
    }
    spin_unlock(&alarm_lock);
    return left;
}

void alarm_cancel(struct proc *p)
{
    alarm_set(p, 0);
}
