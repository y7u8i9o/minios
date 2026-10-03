/* Identity system calls (U0): getresuid, getresgid, setresuid, setresgid,
 * getgroups, setgroups and umask. Credentials belong to the process, so a
 * change made by one thread applies to all of its threads. */
#include <syscall/syscalls.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <lib/string.h>
#include <errno.h>

/* The value -1 of the set calls leaves an id unchanged. */
#define ID_UNCHANGED 0xffffffffu

static long put_ids(uintptr_t a, uintptr_t b, uintptr_t c, uint32_t x, uint32_t y, uint32_t z)
{
    uintptr_t ptrs[3] = { a, b, c };
    uint32_t vals[3] = { x, y, z };
    for (int i = 0; i < 3; i++)
        if (ptrs[i] && !user_range_ok(ptrs[i], sizeof(uint32_t), true))
            return -EFAULT;
    for (int i = 0; i < 3; i++)
        if (ptrs[i])
            memcpy((void *)ptrs[i], &vals[i], sizeof(uint32_t));
    return 0;
}

long sys_getresuid(struct trapframe *tf)
{
    struct cred c;
    cred_get_current(&c);
    return put_ids(SYSARG0(tf), SYSARG1(tf), SYSARG2(tf), c.ruid, c.euid, c.suid);
}

long sys_getresgid(struct trapframe *tf)
{
    struct cred c;
    cred_get_current(&c);
    return put_ids(SYSARG0(tf), SYSARG1(tf), SYSARG2(tf), c.rgid, c.egid, c.sgid);
}

/* An unprivileged process may set each id only to one of its current real,
 * effective or saved ids. Root may set any value. */
static bool id_allowed(uint32_t want, uint32_t r, uint32_t e, uint32_t s)
{
    return want == ID_UNCHANGED || want == r || want == e || want == s;
}

static long set_res(uint32_t r, uint32_t e, uint32_t s, bool group)
{
    struct proc *p = thread_current()->proc;
    spin_lock(&p->lock);
    struct cred *c = &p->cred;
    uint32_t *cr = group ? &c->rgid : &c->ruid;
    uint32_t *ce = group ? &c->egid : &c->euid;
    uint32_t *cs = group ? &c->sgid : &c->suid;
    if (c->euid != 0 &&
        !(id_allowed(r, *cr, *ce, *cs) && id_allowed(e, *cr, *ce, *cs) && id_allowed(s, *cr, *ce, *cs))) {
        spin_unlock(&p->lock);
        return -EPERM;
    }
    if (r != ID_UNCHANGED)
        *cr = r;
    if (e != ID_UNCHANGED)
        *ce = e;
    if (s != ID_UNCHANGED)
        *cs = s;
    spin_unlock(&p->lock);
    return 0;
}

long sys_setresuid(struct trapframe *tf)
{
    return set_res((uint32_t)SYSARG0(tf), (uint32_t)SYSARG1(tf), (uint32_t)SYSARG2(tf), false);
}

long sys_setresgid(struct trapframe *tf)
{
    return set_res((uint32_t)SYSARG0(tf), (uint32_t)SYSARG1(tf), (uint32_t)SYSARG2(tf), true);
}

/* getgroups(size, list): size 0 asks for the count only. */
long sys_getgroups(struct trapframe *tf)
{
    int size = (int)SYSARG0(tf);
    uintptr_t list = SYSARG1(tf);
    if (size < 0)
        return -EINVAL;
    struct cred c;
    cred_get_current(&c);
    if (size == 0)
        return c.ngroups;
    if (size < c.ngroups)
        return -EINVAL;
    if (!user_range_ok(list, (size_t)c.ngroups * sizeof(uint32_t), true))
        return -EFAULT;
    memcpy((void *)list, c.groups, (size_t)c.ngroups * sizeof(uint32_t));
    return c.ngroups;
}

long sys_setgroups(struct trapframe *tf)
{
    int size = (int)SYSARG0(tf);
    uintptr_t list = SYSARG1(tf);
    if (size < 0 || size > NGROUPS_MAX)
        return -EINVAL;
    uint32_t groups[NGROUPS_MAX];
    if (size) {
        if (!user_range_ok(list, (size_t)size * sizeof(uint32_t), false))
            return -EFAULT;
        memcpy(groups, (const void *)list, (size_t)size * sizeof(uint32_t));
    }
    struct proc *p = thread_current()->proc;
    spin_lock(&p->lock);
    if (p->cred.euid != 0) {
        spin_unlock(&p->lock);
        return -EPERM;
    }
    memcpy(p->cred.groups, groups, (size_t)size * sizeof(uint32_t));
    p->cred.ngroups = size;
    spin_unlock(&p->lock);
    return 0;
}

long sys_umask(struct trapframe *tf)
{
    struct proc *p = thread_current()->proc;
    spin_lock(&p->lock);
    uint32_t old = p->cred.umask;
    p->cred.umask = (uint32_t)SYSARG0(tf) & 0777;
    spin_unlock(&p->lock);
    return old;
}
