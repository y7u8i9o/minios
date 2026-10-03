#pragma once
#include <kernel.h>
#include <minios/abi.h>

struct proc;

/* The identity of a process (U0): real, effective and saved user and
 * group ids, the supplementary groups and the file creation mask. A copy
 * lives in struct proc, written under proc.lock and read by taking a
 * snapshot under that lock, because the threads of a process share it and
 * may change it concurrently. */
struct cred {
    uint32_t ruid, euid, suid;
    uint32_t rgid, egid, sgid;
    uint32_t groups[NGROUPS_MAX];
    int ngroups;
    uint32_t umask;
};

/* The identity of the kernel process, inherited by init and every process
 * a boot test starts: root, no supplementary groups, umask 022. */
void cred_init_root(struct cred *c);
/* Snapshot of the credentials of p, or of the calling process. */
void cred_get(struct proc *p, struct cred *out);
void cred_get_current(struct cred *out);
/* True if gid is the effective or a supplementary group of c. */
bool cred_in_group(const struct cred *c, uint32_t gid);

static inline bool cred_is_root(const struct cred *c)
{
    return c->euid == 0;
}
