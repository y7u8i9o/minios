#pragma once
#include <kernel.h>
#include <sync/spinlock.h>
#include <minios/abi.h>

struct file;
struct proc;

/* Install, close, flags and limit are protected by lock.  Descriptor slots
 * are published with release stores and looked up under RCU without lock. */
struct fdtable {
    struct file *fds[OPEN_MAX];
    uint64_t cloexec;               /* bit per descriptor, lock */
    int limit;                      /* slots usable, RLIMIT_NOFILE (M40), lock */
    struct spinlock lock;
};

void fdtable_init(struct fdtable *t);
/* Install f at the lowest free slot at or above min. Consumes the caller's
 * reference on success. Returns the descriptor or -EMFILE. */
int fdtable_install(struct fdtable *t, struct file *f, int min);
/* Install f at exactly fd, closing what was there. Consumes the reference. */
int fdtable_install_at(struct fdtable *t, struct file *f, int fd);
/* Return a referenced file for fd, or NULL. */
struct file *fdtable_get(struct fdtable *t, int fd);
/* Remove fd and drop the table's reference. */
int fdtable_close(struct fdtable *t, int fd);
/* Copy every descriptor for fork. */
void fdtable_copy(struct fdtable *dst, struct fdtable *src);
void fdtable_close_all(struct fdtable *t);
/* Close on exec marks (M23). */
void fdtable_set_cloexec(struct fdtable *t, int fd, bool on);
bool fdtable_get_cloexec(struct fdtable *t, int fd);
void fdtable_close_exec(struct fdtable *t);
/* Number of usable slots (M40). Open descriptors above it stay open. */
void fdtable_set_limit(struct fdtable *t, int limit);
