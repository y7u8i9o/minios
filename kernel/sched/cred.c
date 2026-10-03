/* Process credentials (U0). */
#include <sched/cred.h>
#include <sched/proc.h>
#include <sched/thread.h>
#include <lib/string.h>

void cred_init_root(struct cred *c)
{
    memset(c, 0, sizeof *c);
    c->umask = 022;
}

void cred_get(struct proc *p, struct cred *out)
{
    spin_lock(&p->lock);
    *out = p->cred;
    spin_unlock(&p->lock);
}

void cred_get_current(struct cred *out)
{
    cred_get(thread_current()->proc, out);
}

bool cred_in_group(const struct cred *c, uint32_t gid)
{
    if (c->egid == gid)
        return true;
    for (int i = 0; i < c->ngroups; i++)
        if (c->groups[i] == gid)
            return true;
    return false;
}
