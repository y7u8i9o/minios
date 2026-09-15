#pragma once
/* The Internet socket family (N01): validates the type and protocol of
 * AF_INET sockets and dispatches to the protocol backend registered for
 * them. UDP (N05) registers here; TCP combinations remain
 * EPROTONOSUPPORT until N06 registers the stream backend. */
#include <kernel.h>
#include <ipc/socket.h>

struct inet_protocol {
    int type;                       /* SOCK_STREAM or SOCK_DGRAM */
    int protocol;                   /* IPPROTO_TCP or IPPROTO_UDP */
    int (*create)(struct socket *s);
};

void inet_socket_init(void);
int inet_register_protocol(const struct inet_protocol *p);
