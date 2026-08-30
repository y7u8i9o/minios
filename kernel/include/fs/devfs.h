#pragma once
#include <kernel.h>
#include <fs/vfs.h>

/* devfs is an in memory directory of device nodes registered by drivers.
 * It is mounted on /dev. */
void devfs_init(void);
/* Register a device node. mode carries S_IFCHR or S_IFBLK. priv is stored
 * in the inode for the driver. */
int devfs_register(const char *name, uint32_t mode, const struct file_ops *fops, void *priv,
                   uint64_t size);
