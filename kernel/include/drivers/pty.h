#pragma once
#include <kernel.h>

/* Pseudo terminals: /dev/ptmx hands out a master whose slave is
 * /dev/pts<n>; TIOCGPTN on the master returns n. */
void pty_init(void);
struct file;
/* True if f is an open slave side of a pseudo terminal. */
bool pty_is_slave(const struct file *f);
