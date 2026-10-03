#pragma once
#include <kernel.h>
#include <fs/vfs.h>

/* devfs is an in memory directory of device nodes registered by drivers.
 * It is mounted on /dev. */
void devfs_init(void);
void devfs_log_nodes(void);      /* log the registered nodes once at boot */
/* Register a device node. mode carries S_IFCHR or S_IFBLK. priv is stored
 * in the inode for the driver. */
int devfs_register(const char *name, uint32_t mode, const struct file_ops *fops, void *priv,
                   uint64_t size);
/* Give the node name (in the root of /dev) the permission bits perm and an
 * owner, as chmod and chown would, for drivers that hand a node to the
 * process using it (U2). No inode lock may be held. */
int devfs_set_owner(const char *name, uint32_t perm, uint32_t uid, uint32_t gid);
/* Set the size that stat reports for the node name in the root of /dev,
 * for a partition whose table was read again. No inode lock may be held. */
int devfs_set_size(const char *name, uint64_t size);
