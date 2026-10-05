#pragma once
#include <kernel.h>

/* virtio-9p (V5 of docs/plan/release-0.6.0.md, docs/design/9p.md): the
 * transport of the 9P2000.L client of fs/9p. Each device is a channel with
 * a mount tag. A request is one message buffer for the host and one buffer
 * for the answer. Several requests are pending at once. Each device
 * appears as /dev/9p/TAG. */
struct p9_channel;

void virtio_9p_init(void);
/* The channel of the device with the mount tag, or NULL. */
struct p9_channel *virtio_9p_find(const char *tag);
/* Sends the message of tlen bytes in tbuf and waits for the answer in rbuf
 * (rcap bytes). The transport writes the tag of the request into the
 * message, except for Tversion, which carries NOTAG. Both buffers must be
 * physically contiguous, as kmalloc returns them. Returns the length of
 * the answer, or -EIO when the device fails. Sleeps. */
long virtio_9p_request(struct p9_channel *ch, void *tbuf, size_t tlen, void *rbuf, size_t rcap);
