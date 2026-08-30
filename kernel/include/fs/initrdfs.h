#pragma once
#include <kernel.h>

/* Read only filesystem over the ustar initrd parsed by fs/initrd.c. */
void initrdfs_init(void);
/* fs/mfs: the disk filesystem (M13). */
void mfs_init(void);
