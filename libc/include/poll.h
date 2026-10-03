#pragma once
/* poll and struct pollfd with its POLL* bits, which come from the ABI
 * header and are also declared by sys/ipc.h. */
#include <sys/types.h>
#include <minios/abi.h>

typedef unsigned nfds_t;
int poll(struct pollfd *fds, unsigned n, int timeout_ms);
